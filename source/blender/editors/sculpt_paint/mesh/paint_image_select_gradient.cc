/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>

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
#include "GPU_immediate_util.hh"
#include "GPU_state.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "../paint_gradient_curve.hh"
#include "../paint_intern.hh"
#include "paint_image_select_gradient.hh"
#include "paint_image_select_intern.hh"

namespace blender {

/* Short alias for the curve-polyline namespace nested under sculpt_paint. */
namespace gradient_curve = ed::sculpt_paint::gradient_curve;

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

float image_paint_gradient_remap_midpoint(const float t, const float midpoint)
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
 * Wrap a raw gradient parameter by the repeat mode and bend it by the midpoint. Shared by the
 * generic evaluation switch and the Curve branch of the per-row task.
 */
static float image_paint_gradient_finish_t(const float t_raw,
                                           const ImagePaintGradientRepeat repeat,
                                           const float midpoint)
{
  const float t = image_paint_gradient_sample_t(t_raw, repeat);
  return image_paint_gradient_remap_midpoint(t, midpoint);
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
 *
 * The Curve type ignores #axis_scale: its polyline is built in an already isotropic space (the
 * canvas pixels of the tile it was drawn on), so \a sample must be expressed in that same space
 * by the caller (see the per-row task for the UDIM tile offset). A null curve (nothing drawn
 * yet) and Along-mode points outside the width limit return NaN, meaning "leave untouched".
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
    case ImagePaintGradientType::Curve: {
      if (params.curve == nullptr) {
        /* Nothing drawn yet: a NaN parameter leaves every pixel untouched. */
        return std::numeric_limits<float>::quiet_NaN();
      }
      /* The curve and the sample are both in the curve's own (isotropic) space. */
      t_raw = params.curve->eval_t(eGradientCurveMode(params.curve_mode),
                                   params.curve_width,
                                   sample_in);
      break;
    }
  }

  return image_paint_gradient_finish_t(t_raw, params.repeat, midpoint);
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
  /* The Fill brush has no stroke to draw a curve with: only the Linear / Radial fill modes exist
   * here, and #ImagePaintGradientType::Curve is deliberately unreachable from this path. */
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
 *
 * The pixel weight is the product of the user-authored and the derived face-selection weights
 * (#BKE_image_paint_selection_blend_sample), so the region is the intersection of the bounds of
 * every active source. A face-only selection still narrows the region, otherwise the gradient
 * would rasterize the whole tile for what may be a tiny selected area.
 */
static bool image_paint_gradient_selection_bounds_region(
    const Image *image, const int tile_number, const int tile_w, const int tile_h, rcti &r_region)
{
  const bool use_user_mask = BKE_image_paint_selection_mask_has_any(image);
  const bool use_face_mask = BKE_image_paint_selection_derived_active(image);

  int sel_min[2] = {0, 0}, sel_max[2] = {tile_w, tile_h};
  bool has_selection = use_user_mask || use_face_mask;
  if (use_user_mask) {
    has_selection = BKE_image_paint_selection_mask_bounds(image, tile_number, sel_min, sel_max);
  }
  if (has_selection && use_face_mask) {
    int face_min[2], face_max[2];
    if (BKE_image_paint_selection_face_mask_bounds(image, tile_number, face_min, face_max)) {
      sel_min[0] = std::max(sel_min[0], face_min[0]);
      sel_min[1] = std::max(sel_min[1], face_min[1]);
      sel_max[0] = std::min(sel_max[0], face_max[0]);
      sel_max[1] = std::min(sel_max[1], face_max[1]);
      /* `sel_max` is exclusive. */
      has_selection = sel_min[0] < sel_max[0] && sel_min[1] < sel_max[1];
    }
    else {
      has_selection = false;
    }
  }

  if (!has_selection) {
    BLI_rcti_init(&r_region, 0, 0, 0, 0);
    return false;
  }
  const PaintSelectionEdgePolicy &edge_policy = BKE_image_paint_selection_edge_policy_get(image);
  BKE_image_paint_selection_bounds_expand_for_blend(sel_min, sel_max, tile_w, tile_h, edge_policy);
  /* `sel_max` is already an exclusive upper bound (see #BKE_image_paint_selection_mask_bounds and
   * #BKE_image_paint_selection_bounds_expand_for_blend), so it must not be incremented again here
   * or the region can exceed the tile by one row/column. */
  BLI_rcti_init(&r_region, sel_min[0], sel_max[0], sel_min[1], sel_max[1]);
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
  if (BKE_image_paint_selection_is_active(image)) {
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
 * \note There is deliberately no bounding-box culling against the gradient vector for the
 * vector-based types. That vector only positions the ramp, it does not bound the painted area.
 * With #ImagePaintGradientRepeat::None the parameter is *clamped* to [0, 1] (see
 * #image_paint_gradient_sample_t), so pixels past the drag still receive the ramp's end color
 * rather than being skipped, and Repeat/Reflect wrap it indefinitely. On top of that, Conical
 * parameterizes by the angle around the start point and so sweeps the whole plane, Linear is
 * unbounded perpendicular to its axis, and Diamond/Square reach up to sqrt(2) past the Euclidean
 * radius because they use the L1 and L-infinity metrics. The bounding box previously used here
 * dropped tiles in every one of those cases, leaving them silently unpainted. Culling only
 * becomes sound once the ramp is known not to contribute (a fully transparent end stop, say),
 * which cannot be decided from the gradient geometry alone.
 *
 * The Curve type is the only bounded exception, and only in Along mode with an explicit width:
 * there the parameter is NaN ("leave untouched") beyond that width, so the region can safely
 * shrink to the curve's bounding box plus the width. Across is unbounded like the vector types
 * (the ramp keeps going past the width / clamps its end color), so it is not culled.
 */
static void image_paint_gradient_calc_work_region_uv(const Scene * /*scene*/,
                                                     const Image *image,
                                                     const int tile_number,
                                                     const int tile_w,
                                                     const int tile_h,
                                                     const ImagePaintGradientParams &params,
                                                     const float2 & /*start_uv*/,
                                                     const float2 & /*end_uv*/,
                                                     rcti &r_region)
{
  BLI_rcti_init(&r_region, 0, 0, 0, 0);

  if (BKE_image_paint_selection_is_active(image)) {
    if (image_paint_gradient_selection_bounds_region(image, tile_number, tile_w, tile_h, r_region))
    {
      image_paint_gradient_sanitize_region(r_region, tile_w, tile_h);
    }
    return;
  }

  BLI_rcti_init(&r_region, 0, tile_w, 0, tile_h);
  image_paint_gradient_sanitize_region(r_region, tile_w, tile_h);

  if (params.type == ImagePaintGradientType::Curve && params.curve != nullptr) {
    /* Only Along with an explicit width bounds the influence (NaN beyond it), so the region can
     * shrink to the curve's bounds plus that width. Across is unbounded like the vector types:
     * its parameter keeps growing past the width and the repeat / clamp logic paints the ramp's
     * end color (or repeated bands) everywhere, so the whole tile must be repainted. */
    if (params.curve_mode == GRADIENT_CURVE_MODE_ALONG && params.curve_width > 0.0f) {
      /* Pixels further than the width from the curve return NaN, so nothing outside the curve's
       * bounds plus the width can change. */
      const float2 origin_px =
          (image_select_udim_tile_uv_origin(tile_number) - params.curve_tile_origin_uv) *
          float2(params.curve_tile_size);
      const float2 curve_to_tile_scale = float2(tile_w, tile_h) / float2(params.curve_tile_size);
      const float margin = params.curve_width + 1.0f;
      rcti curve_region;
      BLI_rcti_init(&curve_region,
                    int(std::floor((params.curve_bounds_min.x - origin_px.x - margin) *
                                   curve_to_tile_scale.x)),
                    int(std::ceil((params.curve_bounds_max.x - origin_px.x + margin) *
                                  curve_to_tile_scale.x)),
                    int(std::floor((params.curve_bounds_min.y - origin_px.y - margin) *
                                   curve_to_tile_scale.y)),
                    int(std::ceil((params.curve_bounds_max.y - origin_px.y + margin) *
                                  curve_to_tile_scale.y)));
      BLI_rcti_isect(&r_region, &curve_region, &r_region);
    }
  }
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
  ImagePaintGradientParams params;
  float2 start_px;
  float2 end_px;
  float midpoint;
  /**
   * Curve type: shift from this tile's local pixel space into the curve's own space (the canvas
   * pixels of the tile it was drawn on), see #image_paint_gradient_apply_region.
   */
  float2 curve_offset_px = float2(0.0f);
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
  if (data.params.type == ImagePaintGradientType::Curve && data.params.curve != nullptr) {
    /* The curve lives in the start tile's pixel space; shift this tile's local pixels into it
     * (see #image_paint_gradient_apply_region for how the offset is derived). The empty-curve
     * NaN and the Along width-limit NaN both survive the repeat / midpoint wrapping; Across is
     * unbounded and simply clamps / repeats. */
    const float2 sample = float2(float(px), float(py)) *
                              (float2(data.params.curve_tile_size) /
                               float2(data.canvas_ibuf->x, data.canvas_ibuf->y)) +
                          data.curve_offset_px;
    return image_paint_gradient_finish_t(
        data.params.curve->eval_t(eGradientCurveMode(data.params.curve_mode),
                                  data.params.curve_width,
                                  sample),
        data.params.repeat,
        data.midpoint);
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
  if (std::isnan(t)) {
    /* Curve (Along width limit) or nothing was drawn: leave this pixel untouched. */
    return;
  }
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
  if (std::isnan(t)) {
    /* Curve (Along width limit) or nothing was drawn: leave this pixel untouched. */
    return;
  }
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
 * responsible for filling the tile-local pixel coordinate fields.
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
  task_data.use_selection_mask = BKE_image_paint_selection_is_active(image);
  task_data.params = params;
  task_data.start_px = start_px;
  task_data.end_px = end_px;
  task_data.midpoint = midpoint;
  task_data.canvas_ibuf = canvas_ibuf;
  task_data.work_region = work_region;

  if (params.type == ImagePaintGradientType::Curve) {
    /* The curve pixel coordinates use the dimensions of its originating tile, including when
     * this UDIM tile has a different resolution. */
    task_data.curve_offset_px = (image_select_udim_tile_uv_origin(tile_number) -
                                 params.curve_tile_origin_uv) *
                                float2(params.curve_tile_size);
  }

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
 * \param painted_region In/out: tile-local pixels already modified by previous preview passes.
 *                       The on-screen part is restored before repainting; the off-screen part is
 *                       left for a later pass. Updated to reflect what this pass leaves dirty.
 * \param r_work_region  Output: the pixel region that was restored (and, where it intersects the
 *                       work region, painted). Empty when there is nothing to do for the tile.
 *
 * \a backup_ibuf is the full-tile, pixel-identical copy of the canvas captured at session start
 * (coordinates are 1:1 with \a canvas_ibuf). The function restores only the pixels it is about to
 * repaint, then applies the gradient. Restoring from the original every frame keeps each blend a
 * fresh composite over the unmodified texture, so non-`MIX` blend modes cannot accumulate. It
 * also restores the still-visible part of the *previous* pass's region, so a shrunken work region
 * (a narrower width, a moved or removed curve, a mode switch) cannot leave a stale gradient behind.
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
                                                  rcti &painted_region,
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

  /* Restore the region about to be repainted plus what earlier passes left dirty and are still
   * on-screen. On a viewport-clipped pass the off-screen part of the history is deliberately left
   * alone: it is invisible, so restoring it would only clean pixels we are not going to repaint,
   * leaving them blank until the next full pass. Keeping it painted lets the pass that later
   * scrolls it into view restore + repaint it correctly, and the full pass settles the rest. A
   * zeroed rectangle is not empty for #BLI_rcti_union, so only real rects are combined. */
  rcti restore_region = work_region;
  if (!BLI_rcti_is_empty(&painted_region)) {
    rcti hist = painted_region;
    if (viewport_clip && !BLI_rcti_is_empty(viewport_clip)) {
      BLI_rcti_isect(&hist, viewport_clip, &hist);
    }
    if (!BLI_rcti_is_empty(&hist)) {
      if (BLI_rcti_is_empty(&restore_region)) {
        restore_region = hist;
      }
      else {
        BLI_rcti_union(&restore_region, &hist);
      }
    }
  }
  r_work_region = restore_region;

  if (BLI_rcti_is_empty(&restore_region)) {
    return;
  }

  /* The backup is the full tile, so coordinates are 1:1 with the canvas. */
  if (backup_ibuf) {
    IMB_copy_rect(canvas_ibuf,
                  backup_ibuf,
                  int2(restore_region.xmin, restore_region.ymin),
                  int2(restore_region.xmin, restore_region.ymin),
                  int2(BLI_rcti_size_x(&restore_region), BLI_rcti_size_y(&restore_region)));
  }

  if (!BLI_rcti_is_empty(&work_region)) {
    /* The gradient vector is stored in global UDIM UV space so its handles remain continuous
     * across tiles. Convert it to this tile's pixel space before rasterizing, matching the
     * established 2D image-paint gradient path. In particular, this avoids the separate global-UV
     * rasterizer that left tiled ImBufs unchanged. */
    const float2 tile_origin = image_select_udim_tile_uv_origin(tile_number);
    const float2 start_px((start_uv.x - tile_origin.x) * canvas_ibuf->x,
                          (start_uv.y - tile_origin.y) * canvas_ibuf->y);
    const float2 end_px((end_uv.x - tile_origin.x) * canvas_ibuf->x,
                        (end_uv.y - tile_origin.y) * canvas_ibuf->y);
    image_paint_gradient_apply_region(
        scene, image, tile_number, canvas_ibuf, params, start_px, end_px, midpoint, work_region);
  }

  /* Track what is left dirty. A full pass repaints everything it restored, so the dirty set is
   * exactly the work region. A viewport-clipped pass leaves the off-screen history untouched (it
   * still holds the old preview), so keep the union until a pass covers it. */
  if (viewport_clip && !BLI_rcti_is_empty(viewport_clip)) {
    if (!BLI_rcti_is_empty(&work_region)) {
      if (BLI_rcti_is_empty(&painted_region)) {
        painted_region = work_region;
      }
      else {
        BLI_rcti_union(&painted_region, &work_region);
      }
    }
  }
  else {
    painted_region = work_region;
  }
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
  /**
   * Tile-local pixels this preview has already modified but not yet restored from #undo_ibuf. A
   * preview pass restores `painted_region ∪ new work region` before repainting, so pixels painted
   * by an earlier pass outside the new (possibly smaller) work region -- a narrower width, a moved
   * curve, a mode switch -- are rolled back instead of leaving a stale gradient on the canvas.
   */
  rcti painted_region = {0, 0, 0, 0};
};

/**
 * Runtime state of a floating gradient preview.
 *
 * Derives from the shared base like the three lifted-fragment tools, even though it uses only part
 * of it: #owner_sima, #owner_region_type and #draw_handle used to be duplicated here verbatim.
 * #iuser is unused (each backed tile carries its own #ImageSelectGradientTileData::iuser), and
 * #is_dragging is unused (the drag lives in the operator's #GradientDragData).
 */
struct ImageSelectGradientState : public PaintSelectFloatingSession {
  static constexpr PaintSelectTool tool_type = PaintSelectTool::Gradient;
  ImageSelectGradientState() : PaintSelectFloatingSession(tool_type) {}

  Vector<ImageSelectGradientTileData> tiles;
  int start_tile_number = 1001;

  float2 start_uv = {0.0f, 0.0f};
  float2 end_uv = {0.0f, 0.0f};
  float midpoint = 0.5f;

  /* Curve gradient (`IMAGE_PAINT_GRADIENT_CURVE`). The raw points are collected in global UV, the
   * resampled curve lives in the canvas pixels of #start_tile_number (see the helpers below);
   * null until the drag has produced at least two distinct points. The raw points are the
   * editable control points of the curve (selection and hover are indices into them). */
  Vector<float2> curve_points_uv;
  std::unique_ptr<gradient_curve::GradientCurve2D> curve;
  /** Indices into #curve_points_uv of the currently selected control points. */
  Vector<int> curve_selected_points;
  /** Control point under the mouse, for the overlay highlight (-1 when none). */
  int curve_hover_point = -1;
  /** Set while a brand-new stroke is being drawn: the raw points are still hundreds of samples
   * long, so the overlay hides the control-point markers until the stroke is released. */
  bool curve_is_drawing = false;
  /** Pixel dimensions of the starting tile, for the UV <-> curve-space conversions. */
  int2 tile_size = int2(1024);

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
  BLI_rcti_init(&tile.painted_region, 0, 0, 0, 0);
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

/* -------------------------------------------------------------------- */
/** \name Curve gradient session helpers
 * \{ */

/** Canvas pixels of the tile the curve was drawn on, from a global UV point. */
static float2 image_select_gradient_curve_px_from_uv(const ImageSelectGradientState *state,
                                                     const float2 &uv)
{
  /* The curve is intentionally measured in its starting tile's pixels. Other tiles may have
   * different resolutions; their pixel-coordinate offsets are resolved independently at apply. */
  return (uv - image_select_udim_tile_uv_origin(state->start_tile_number)) *
         float2(state->tile_size);
}

/** Global UV point from the canvas pixels of the tile the curve was drawn on. */
static float2 image_select_gradient_curve_uv_from_px(const ImageSelectGradientState *state,
                                                     const float2 &px)
{
  return image_select_udim_tile_uv_origin(state->start_tile_number) +
         px / float2(state->tile_size);
}

/** Position of the midpoint handle: on the curve for the Curve type, on the vector otherwise. */
static float2 image_select_gradient_mid_uv_for_type(const ImageSelectGradientState *state,
                                                    const bool is_curve)
{
  if (is_curve && state->curve != nullptr) {
    const float2 mid_px = state->curve->position_at_s(state->midpoint * state->curve->length());
    return image_select_gradient_curve_uv_from_px(state, mid_px);
  }
  return image_select_gradient_mid_uv(state);
}

/** Whether the session's gradient is the Curve type (read live, it can change mid-session). */
static bool image_select_gradient_is_curve(const Scene *scene)
{
  return scene != nullptr &&
         scene->toolsettings->imapaint.gradient_type == IMAGE_PAINT_GRADIENT_CURVE;
}

/**
 * Rebuild the resampled curve from the collected UV points, in the start tile's pixel space.
 * An invalid or not-yet-drawn curve is stored as null: the evaluation then leaves every pixel
 * untouched instead of painting a degenerate ramp.
 */
static void image_select_gradient_curve_rebuild(ImageSelectGradientState *state,
                                                const ImagePaintSettings &imapaint)
{
  if (state->curve_points_uv.size() < 2) {
    state->curve = nullptr;
    return;
  }
  Vector<float2> raw_px;
  raw_px.reserve(state->curve_points_uv.size());
  for (const float2 &uv : state->curve_points_uv) {
    raw_px.append(image_select_gradient_curve_px_from_uv(state, uv));
  }
  auto curve = std::make_unique<gradient_curve::GradientCurve2D>();
  /* Even ~3 pixel spacing over the canvas is far below what a color ramp can resolve, and the
   * curve builder caps the point count for very long strokes. */
  const gradient_curve::GradientCurveBuildParams params = {
      /*spacing=*/3.0f,
      /*smooth=*/imapaint.gradient_curve_smooth,
      gradient_curve::default_smooth_iterations,
  };
  curve->build(raw_px, params);
  state->curve = curve->is_valid() ? std::move(curve) : nullptr;
}

/**
 * Reduce the raw samples collected during a drag to a small, editable set of control points: RDP
 * in the curve's own pixel space (isotropic, so a non-square tile does not distort the metric),
 * keeping the corners and curvature extremes a hand-placed curve would have. Without this the
 * overlay body, hit-testing and per-point editing would each work on hundreds of points spaced a
 * few pixels apart.
 */
static void image_select_gradient_curve_simplify(ImageSelectGradientState *state,
                                                 const ImagePaintSettings &imapaint)
{
  if (state->curve_points_uv.size() <= 2) {
    return;
  }
  Array<float2> raw_px(state->curve_points_uv.size());
  for (const int i : state->curve_points_uv.index_range()) {
    raw_px[i] = image_select_gradient_curve_px_from_uv(state, state->curve_points_uv[i]);
  }
  const Vector<int> kept = gradient_curve::simplify_control_points<float2>(
      raw_px, gradient_curve::simplify_tolerance_px, gradient_curve::max_control_points);
  if (kept.size() >= state->curve_points_uv.size()) {
    return;
  }
  Vector<float2> reduced;
  reduced.reserve(kept.size());
  for (const int index : kept) {
    reduced.append(state->curve_points_uv[index]);
  }
  state->curve_points_uv = std::move(reduced);
  /* The old selection / hover indices no longer match the reduced list. */
  state->curve_selected_points.clear();
  state->curve_hover_point = -1;
  image_select_gradient_curve_rebuild(state, imapaint);
}

/** Hit radius (region pixels) for curve control points and the curve body. Sized to match the
 * enlarged control-point markers. */
static constexpr float GRADIENT_CURVE_HIT_RADIUS_PX = 12.0f;
/** Marker radii (region pixels) of the editable curve control points. */
static constexpr float GRADIENT_CURVE_POINT_RADIUS_PX = 5.25f;
static constexpr float GRADIENT_CURVE_POINT_HOVER_RADIUS_PX = 7.5f;

enum class GradientCurveHitType {
  /** No curve feature under the mouse. */
  None,
  /** A control point (including the two endpoints) is under the mouse. */
  ControlPoint,
  /** The mouse is on the curve between control points. */
  CurveBody,
};

struct GradientCurveHitResult {
  GradientCurveHitType type = GradientCurveHitType::None;
  /** Control point index, for #GradientCurveHitType::ControlPoint. */
  int point_index = -1;
  /** Arc-length position (curve space) of the nearest point on the curve, for the body. */
  float nearest_s = 0.0f;
};

/**
 * Curve hit-testing for the per-point editing: the nearest control point within the hit radius
 * wins; otherwise the mouse projecting onto the drawn curve within the radius hits the body.
 * Screen distances are measured in region pixels so the hit radius is zoom independent.
 */
static GradientCurveHitResult image_select_gradient_curve_hit_test(
    const ARegion *region, const wmEvent *event, const ImageSelectGradientState *state)
{
  GradientCurveHitResult hit;
  if (region == nullptr || state->curve_points_uv.is_empty()) {
    return hit;
  }
  /* Window-absolute to region-relative, like the handle hit-test: #wmEvent.mval cannot be
   * trusted while the modal runs. */
  const float mx = float(event->xy[0] - region->winrct.xmin);
  const float my = float(event->xy[1] - region->winrct.ymin);
  const float2 mouse_px(mx, my);

  int nearest_i = -1;
  float nearest_dist_sq = GRADIENT_CURVE_HIT_RADIUS_PX * GRADIENT_CURVE_HIT_RADIUS_PX;
  for (const int i : state->curve_points_uv.index_range()) {
    float rx, ry;
    ui::view2d_view_to_region_fl(
        &region->v2d, state->curve_points_uv[i].x, state->curve_points_uv[i].y, &rx, &ry);
    const float dist_sq = math::distance_squared(float2(rx, ry), mouse_px);
    if (dist_sq <= nearest_dist_sq) {
      nearest_dist_sq = dist_sq;
      nearest_i = i;
    }
  }
  if (nearest_i >= 0) {
    hit.type = GradientCurveHitType::ControlPoint;
    hit.point_index = nearest_i;
    return hit;
  }

  if (state->curve != nullptr && state->curve->is_valid()) {
    float ux, uy;
    ui::view2d_region_to_view(&region->v2d, mx, my, &ux, &uy);
    const float2 mouse_curve_px = image_select_gradient_curve_px_from_uv(state, float2(ux, uy));
    const gradient_curve::CurveProjection proj = state->curve->project(mouse_curve_px);
    const float2 proj_uv = image_select_gradient_curve_uv_from_px(
        state, state->curve->position_at_s(proj.s));
    float rx, ry;
    ui::view2d_view_to_region_fl(&region->v2d, proj_uv.x, proj_uv.y, &rx, &ry);
    if (math::distance(float2(rx, ry), mouse_px) <= GRADIENT_CURVE_HIT_RADIUS_PX) {
      hit.type = GradientCurveHitType::CurveBody;
      hit.nearest_s = proj.s;
    }
  }
  return hit;
}

/**
 * Index in #curve_points_uv at which a control point at \a uv keeps the point list ordered along
 * the curve: the raw segment the position is closest to, so the point is inserted between its
 * two neighbors.
 */
static int image_select_gradient_curve_insert_index(const ImageSelectGradientState *state,
                                                    const float2 &uv)
{
  const int points_num = state->curve_points_uv.size();
  int best_i = points_num;
  float best_dist_sq = FLT_MAX;
  for (const int i : IndexRange(1, points_num - 1)) {
    const float dist_sq = dist_squared_to_line_segment_v2(
        uv, state->curve_points_uv[i - 1], state->curve_points_uv[i]);
    if (dist_sq < best_dist_sq) {
      best_dist_sq = dist_sq;
      best_i = i;
    }
  }
  return best_i;
}

/** Remove every selected control point (if any), clear the selection and rebuild the curve. */
static void image_select_gradient_curve_remove_selected(ImageSelectGradientState *state,
                                                        const ImagePaintSettings &imapaint)
{
  Vector<int> &selected = state->curve_selected_points;
  if (selected.is_empty()) {
    return;
  }
  /* Highest index first, so the removals below keep the remaining indices valid. */
  Array<int> sorted(selected.as_span());
  std::sort(sorted.begin(), sorted.end(), std::greater<int>());
  for (const int index : sorted) {
    if (index >= 0 && index < state->curve_points_uv.size()) {
      state->curve_points_uv.remove(index);
    }
  }
  selected.clear();
  state->curve_hover_point = -1;
  image_select_gradient_curve_rebuild(state, imapaint);
}

/** \} */

/**
 * Runtime parameters of the floating gradient session: the tool settings, plus the drawn curve
 * for the Curve type (null until at least two points have been collected).
 */
static ImagePaintGradientParams image_select_gradient_current_params(
    const Scene *scene, const ImageSelectGradientState *state)
{
  ImagePaintGradientParams params = image_paint_gradient_params_from_imapaint(
      scene->toolsettings->imapaint);
  if (params.type == ImagePaintGradientType::Curve && state != nullptr &&
      state->curve != nullptr)
  {
    params.curve = state->curve.get();
    params.curve_mode = eGradientCurveMode(scene->toolsettings->imapaint.gradient_curve_mode);
    params.curve_width = scene->toolsettings->imapaint.gradient_curve_width;
    params.curve_tile_origin_uv = image_select_udim_tile_uv_origin(state->start_tile_number);
    params.curve_tile_size = state->tile_size;
    const blender::Bounds<float2> bounds = state->curve->bounds();
    params.curve_bounds_min = bounds.min;
    params.curve_bounds_max = bounds.max;
  }
  return params;
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

/* One-way sync of the drawn curve into the operator's hidden `curve_points` collection (global
 * UV per point), so the last drag is inspectable from the redo panel and from Python. The
 * floating session stays the source of truth for the preview. */
static void image_select_gradient_sync_curve_to_op(wmOperator *op,
                                                   const ImageSelectGradientState *state)
{
  RNA_collection_clear(op->ptr, "curve_points");
  if (state == nullptr) {
    return;
  }
  for (const float2 &uv : state->curve_points_uv) {
    PointerRNA itemptr;
    RNA_collection_add(op->ptr, "curve_points", &itemptr);
    RNA_float_set_array(&itemptr, "loc", uv);
  }
}

static bool image_select_gradient_mouse_to_global_uv(
    const ARegion *region, Image *ima, const wmEvent *event, int &r_tile_number, float2 &r_uv)
{
  /* Derive region-relative coordinates from the window-absolute #wmEvent.xy. While the modal runs
   * the context region (and therefore #wmEvent.mval) may belong to the header or N-panel when the
   * pointer leaves the image; #wmEvent.xy stays valid and keeps the gradient anchored to the
   * image region passed in here. */
  const float mx = float(event->xy[0] - region->winrct.xmin);
  const float my = float(event->xy[1] - region->winrct.ymin);

  float uv[2];
  ui::view2d_region_to_view(&region->v2d, mx, my, &uv[0], &uv[1]);

  /* #BKE_image_get_tile_from_pos writes tile-local coordinates to its second argument. Keep the
   * view coordinate untouched: gradient handles and rasterization use global UVs, especially when
   * the image is tiled. */
  float local_uv[2];
  float uv_origin[2];
  int tile_number = BKE_image_get_tile_from_pos(ima, uv, local_uv, uv_origin);
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
    const int start_tile_number,
    const float2 & /*end_uv*/,
    Vector<int> &r_tile_numbers)
{
  r_tile_numbers.clear();
  if (!ima) {
    return;
  }

  const bool use_selection_mask = BKE_image_paint_selection_is_active(ima);
  /* Only a user-authored selection has meaningful per-tile bounding boxes. The derived
   * face-selection mask is per-tile as well but blocks the pixels it doesn't cover per pixel, so
   * skipping a tile by user-mask bounds would drop tiles that only the derived mask selects. */
  const bool filter_tiles_by_user_bounds = use_selection_mask &&
                                           !BKE_image_paint_selection_derived_active(ima);
  /* Without the multi-UDIM option the gradient is confined to the tile where the drag started. The
   * tile is stored in the session rather than derived from start_uv: moving the start handle
   * across a UDIM boundary must not switch the canvas backup or selection mask to another tile. */
  const bool multi_udim = scene && scene->toolsettings->imapaint.gradient_multi_udim;
  int active_tile_number = 1001;
  if (!multi_udim) {
    const ImageTile *active_tile = BKE_image_get_tile(ima, start_tile_number);
    if (!active_tile) {
      active_tile = static_cast<const ImageTile *>(
          BLI_findlink(&ima->tiles, ima->active_tile_index));
    }
    if (active_tile) {
      active_tile_number = active_tile->tile_number;
    }
  }

  for (const ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
    if (!multi_udim && tile->tile_number != active_tile_number) {
      continue;
    }
    if (filter_tiles_by_user_bounds) {
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
  /* The fresh backup equals the canvas, so nothing is dirty yet. */
  BLI_rcti_init(&tile.painted_region, 0, 0, 0, 0);

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
  if (C) {
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
  }
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
      ima, scene, params, state->start_tile_number, state->end_uv, needed_tiles);

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
  const ARegion *vp_region = (use_viewport_clip && !commit_to_image) ? CTX_wm_region(C) : nullptr;
  bool gpu_textures_changed = false;

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

    /* Apply preview; work_region is the union of the previous painted region and the current
     * work region -- both restored, the current one also painted. Marking the whole union keeps
     * the GPU texture coherent in the part that was only cleaned. */
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
                                          tile.painted_region,
                                          work_region);

    if (commit_to_image) {
      BKE_image_mark_dirty(ima, ibuf);
    }
    else {
      /* Preview path: invalidate the display buffer, but do NOT mark_dirty. */
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    }

    if (!BLI_rcti_is_empty(&work_region)) {
      const ImageTile *itile = BKE_image_get_tile(ima, tile.tile_number);
      BKE_image_partial_update_mark_region(ima, itile, ibuf, &work_region);
      gpu_textures_changed = true;
    }

    BKE_image_release_ibuf(ima, ibuf, lock);
  }

  if (gpu_textures_changed) {
    /* Partial-update registration is consumed asynchronously and does not itself upload pixels.
     * Releasing the cached texture forces the Image Editor to recreate its UDIM texture array from
     * the preview ImBufs on the following redraw. */
    BKE_image_free_gputextures(ima);
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
  if (settings_changed) {
    /* Curve mode / width are re-read from the tool settings on every render, but the smoothing
     * setting changes the curve itself, so it needs a rebuild from the stored points. */
    const Scene *settings_scene = CTX_data_scene(C);
    if (image_select_gradient_is_curve(settings_scene) && state->curve_points_uv.size() >= 2) {
      image_select_gradient_curve_rebuild(state, settings_scene->toolsettings->imapaint);
    }
  }
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
      C, state, image_select_gradient_current_params(scene, state), false, /*use_viewport_clip=*/!force);
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

static void draw_gradient_vector_overlay(const bContext *C, ARegion *region, void *arg)
{
  ImageSelectGradientState *state = static_cast<ImageSelectGradientState *>(arg);
  if (!state) {
    return;
  }

  const Scene *scene = CTX_data_scene(C);
  const bool is_curve = (scene != nullptr) &&
                        scene->toolsettings->imapaint.gradient_type == IMAGE_PAINT_GRADIENT_CURVE;

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  if (is_curve) {
    /* The drawn polyline replaces the straight gradient vector; the start / end markers sit at
     * its ends and the midpoint diamond at its arc-length position. */
    if (state->curve == nullptr) {
      immUnbindProgram();
      GPU_blend(GPU_BLEND_NONE);
      GPU_line_smooth(false);
      return;
    }

    Vector<float2> region_pts;
    region_pts.reserve(state->curve->points_num());
    for (const float2 &px : state->curve->points()) {
      const float2 uv = image_select_gradient_curve_uv_from_px(state, px);
      float rx, ry;
      ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &rx, &ry);
      region_pts.append(float2(rx, ry));
    }

    for (const int pass : IndexRange(2)) {
      GPU_line_width((pass == 0) ? 3.0f : 1.5f);
      if (pass == 0) {
        immUniformColor4f(0.0f, 0.0f, 0.0f, 0.55f);
      }
      else {
        immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
      }
      immBegin(GPU_PRIM_LINE_STRIP, region_pts.size());
      for (const float2 &co : region_pts) {
        immVertex2f(pos, co.x, co.y);
      }
      immEnd();
    }

    const float2 mid_px = state->curve->position_at_s(state->midpoint * state->curve->length());
    const float2 mid_uv = image_select_gradient_curve_uv_from_px(state, mid_px);

    const float p0[2] = {region_pts.first().x, region_pts.first().y};
    const float p1[2] = {region_pts.last().x, region_pts.last().y};
    float pm[2];
    ui::view2d_view_to_region_fl(&region->v2d, mid_uv.x, mid_uv.y, &pm[0], &pm[1]);

    GPU_line_width(3.0f);
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
    draw_gradient_polyline_circle(pos, p0, 7.0f, 24);
    draw_gradient_polyline_circle(pos, p1, 9.0f, 24);
    draw_gradient_diamond(pos, pm, 6.0f);

    GPU_line_width(1.5f);
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.95f);
    draw_gradient_polyline_circle(pos, p0, 7.0f, 24);
    immUniformColor4f(1.0f, 0.85f, 0.0f, 0.95f);
    draw_gradient_polyline_circle(pos, p1, 9.0f, 24);
    immUniformColor4f(0.2f, 0.9f, 1.0f, 0.95f);
    draw_gradient_diamond(pos, pm, 6.0f);

    /* Control points of the raw polyline (editable): hollow circles, filled when selected and
     * enlarged when hovered. Endpoints keep their bigger start / end markers above and still
     * get their selection fill here. Hidden while a new stroke is being drawn: the raw points are
     * still hundreds of unsimplified samples that would bury the curve. */
    if (!state->curve_is_drawing) {
      for (const int i : state->curve_points_uv.index_range()) {
        const bool selected = state->curve_selected_points.contains(i);
        const bool hovered = (state->curve_hover_point == i);
        const float radius = hovered ? GRADIENT_CURVE_POINT_HOVER_RADIUS_PX :
                                       GRADIENT_CURVE_POINT_RADIUS_PX;
        float cx, cy;
        ui::view2d_view_to_region_fl(
            &region->v2d, state->curve_points_uv[i].x, state->curve_points_uv[i].y, &cx, &cy);
        if (selected) {
          immUniformColor4f(0.2f, 0.9f, 1.0f, 0.55f);
          imm_draw_circle_fill_2d(pos, cx, cy, radius + 1.0f, 16);
        }
        GPU_line_width(3.0f);
        immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
        imm_draw_circle_wire_2d(pos, cx, cy, radius, 16);
        GPU_line_width(1.5f);
        immUniformColor4f(1.0f, 1.0f, 1.0f, 0.95f);
        imm_draw_circle_wire_2d(pos, cx, cy, radius, 16);
      }
    }

    immUnbindProgram();
    GPU_blend(GPU_BLEND_NONE);
    GPU_line_smooth(false);
    return;
  }

  /* Draw handles even when tiles is empty (degenerate/zero-length gradient at drag start). */

  const float2 mid_uv = image_select_gradient_mid_uv(state);

  float p0[2], p1[2], pm[2];
  ui::view2d_view_to_region_fl(&region->v2d, state->start_uv.x, state->start_uv.y, &p0[0], &p0[1]);
  ui::view2d_view_to_region_fl(&region->v2d, state->end_uv.x, state->end_uv.y, &p1[0], &p1[1]);
  ui::view2d_view_to_region_fl(&region->v2d, mid_uv.x, mid_uv.y, &pm[0], &pm[1]);

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
  state->start_tile_number = tile_number;
  state->start_uv = start_uv;
  state->end_uv = start_uv;
  state->midpoint = 0.5f;
  state->applied_settings_revision = image_paint_gradient_settings_revision;
  /* Remember the owning tool so the modal can commit when the user switches to another one. */
  if (const bToolRef *tref = WM_toolsystem_ref_from_context(C)) {
    STRNCPY(state->owner_tool_idname, tref->idname);
  }

  Scene *scene = CTX_data_scene(C);
  /* Refresh the derived face-selection masks before the session collects its tiles and starts
   * previewing: otherwise a face-selection-only mask would be ignored until the commit, so the
   * drag preview would neither show nor clip by it. */
  image_paint_selection_mask_from_face_selection(C, scene, ima);
  const ImagePaintGradientParams params = image_select_gradient_current_params(scene, state);
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

  /* Pixel dimensions of the originating tile define the curve's pixel-space conversions. */
  if (ImageSelectGradientTileData *start_tile = image_select_gradient_find_tile(
          state, state->start_tile_number))
  {
    if (start_tile->undo_ibuf != nullptr) {
      state->tile_size = int2(start_tile->undo_ibuf->x, start_tile->undo_ibuf->y);
    }
  }

  ARegion *region = CTX_wm_region(C);
  if (region && region->runtime->type) {
    state->owner_region_type = region->runtime->type;
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, draw_gradient_vector_overlay, state, REGION_DRAW_POST_PIXEL);
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
   * the preview into the image and pushes a complete undo step, so the visible result is kept.
   * Like the confirm path, this applies before the session slot is cleared. Explicit cancel (ESC)
   * still restores the per-tile backups. */
  image_select_gradient_apply_session(C, state);
  image_select_session_clear(sima);
  image_select_gradient_state_free(state);
}

void image_select_gradient_session_cancel(bContext *C, SpaceImage *sima)
{
  ImageSelectGradientState *state = image_select_gradient_state_get(sima);
  if (!state) {
    return;
  }
  image_select_session_clear(sima);
  if (C) {
    image_select_floating_drag_end(C, state);
    image_select_floating_status_clear(C);
  }
  image_select_gradient_restore_session_backup(C, state);
  image_select_gradient_state_free(state);
}

static void image_select_gradient_apply_session(bContext *C, ImageSelectGradientState *state)
{
  Image *ima = state->owner_sima->image;
  Scene *scene = CTX_data_scene(C);
  /* Face selection masking: rebuild the image's derived 2D selection masks from the canvas
   * objects' face selections before the mask state is read below. The derived state is Active only
   * when at least one object has an active selection; with no active selection the masks are freed
   * and painting is unrestricted (a no-op with the flag off). */
  image_paint_selection_mask_from_face_selection(C, scene, ima);
  const ImagePaintGradientParams params = image_select_gradient_current_params(scene, state);

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
  /** Curve: dragging the selected control point(s) of an already drawn curve. */
  PointDrag,
};

struct GradientDragData {
  GradientDragMode drag_mode;
  wmTimer *timer = nullptr;
  /* Curve per-point drag: the selected point indices, their pre-drag positions and the press
   * position, plus the region-space position of the last collected point for the sampling
   * threshold. */
  Vector<int> drag_points;
  Vector<float2> drag_orig_points;
  float2 drag_press_uv = float2(0.0f);
  float2 last_collect_px = float2(0.0f);
};

static constexpr float GRADIENT_HANDLE_HIT_RADIUS_PX = 15.0f;
/** Minimum cursor travel (region pixels) before the next curve sample is accepted. */
static constexpr float GRADIENT_CURVE_COLLECT_DIST_PX = 4.0f;

/**
 * Resolve a Curve-mode press over the floating session: grab the midpoint handle, select and drag
 * control points (Shift toggles multi-selection), insert a point when pressing the curve body
 * (Ctrl inserts without grabbing), or start a brand-new curve on empty space. Returns the drag
 * mode the session should enter.
 */
static GradientDragMode image_select_gradient_curve_press_resolve(const ARegion *region,
                                                                  Image *ima,
                                                                  const wmEvent *event,
                                                                  Scene *scene,
                                                                  ImageSelectGradientState *state,
                                                                  GradientDragData *data);

/** Reset the curve drag to a fresh single-point curve at \a uv; \a r_px gets its region position. */
static void image_select_gradient_curve_start(ImageSelectGradientState *state,
                                              const float2 &uv,
                                              const ARegion *region,
                                              float2 &r_px)
{
  state->curve_points_uv.clear();
  state->curve = nullptr;
  state->curve_selected_points.clear();
  state->curve_hover_point = -1;
  state->curve_points_uv.append(uv);
  float rx, ry;
  ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &rx, &ry);
  r_px = float2(rx, ry);
  /* Keep the vector handles consistent with the still-empty curve. */
  state->start_uv = uv;
  state->end_uv = uv;
  state->midpoint = 0.5f;
}

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

static GradientDragMode image_select_gradient_curve_press_resolve(const ARegion *region,
                                                                  Image *ima,
                                                                  const wmEvent *event,
                                                                  Scene *scene,
                                                                  ImageSelectGradientState *state,
                                                                  GradientDragData *data)
{
  const GradientCurveHitResult hit = image_select_gradient_curve_hit_test(region, event, state);
  if (hit.type == GradientCurveHitType::None) {
    if (image_select_gradient_hit_test_handle(
            region, event, image_select_gradient_mid_uv_for_type(state, true)))
    {
      return GradientDragMode::Mid;
    }
    /* Empty space: start a brand-new curve from this point. On a UV miss (out of the canvas)
     * there is nothing to start from, but the mode is still New: clear the previous curve so the
     * drag does not keep editing it. */
    int tile_number = 0;
    float2 uv;
    if (image_select_gradient_mouse_to_global_uv(region, ima, event, tile_number, uv)) {
      image_select_gradient_curve_start(state, uv, region, data->last_collect_px);
    }
    else {
      state->curve_points_uv.clear();
      state->curve = nullptr;
      state->curve_selected_points.clear();
      state->curve_hover_point = -1;
    }
    state->curve_is_drawing = true;
    return GradientDragMode::New;
  }

  int tile_number = 0;
  float2 press_uv;
  image_select_gradient_mouse_to_global_uv(region, ima, event, tile_number, press_uv);

  if (hit.type == GradientCurveHitType::ControlPoint) {
    const int idx = hit.point_index;
    Vector<int> &selected = state->curve_selected_points;
    if (event->modifier & KM_SHIFT) {
      if (selected.contains(idx)) {
        /* Shift-click on a selected point deselects it and starts no drag. */
        selected.remove_first_occurrence_and_reorder(idx);
        return GradientDragMode::Idle;
      }
      selected.append(idx);
    }
    else if (!selected.contains(idx)) {
      /* Clicking an unselected point selects just that one; a selected one keeps the whole
       * selection so the group can be dragged together. */
      selected.reinitialize(1);
      selected[0] = idx;
    }
    /* Start dragging the whole selection. */
    data->drag_points = selected;
    data->drag_press_uv = press_uv;
    data->drag_orig_points = state->curve_points_uv;
    return GradientDragMode::PointDrag;
  }

  /* Curve body: insert a new control point at the nearest position on the curve, between its
   * future neighbors in the raw point list. */
  BLI_assert(state->curve != nullptr);
  const float2 insert_uv = image_select_gradient_curve_uv_from_px(
      state, state->curve->position_at_s(hit.nearest_s));
  const int insert_index = image_select_gradient_curve_insert_index(state, insert_uv);
  state->curve_points_uv.insert(insert_index, insert_uv);
  image_select_gradient_curve_rebuild(state, scene->toolsettings->imapaint);
  state->curve_selected_points.reinitialize(1);
  state->curve_selected_points[0] = insert_index;
  if (event->modifier & KM_CTRL) {
    /* Ctrl-click inserts without grabbing. */
    return GradientDragMode::Idle;
  }
  data->drag_points = state->curve_selected_points;
  data->drag_press_uv = press_uv;
  data->drag_orig_points = state->curve_points_uv;
  return GradientDragMode::PointDrag;
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
  Scene *scene = CTX_data_scene(C);
  const bool is_curve = image_select_gradient_is_curve(scene);
  if (!is_curve && existing) {
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
                 region, event, image_select_gradient_mid_uv_for_type(existing, is_curve)))
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
  else if (!is_curve && existing) {
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
  if (is_curve) {
    /* The Curve press resolves per point / curve body / midpoint / new curve; the session is
     * guaranteed to exist here (created above when absent). */
    data->drag_mode = image_select_gradient_curve_press_resolve(
        region, ima, event, scene, existing, data);
  }
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
  const ARegion *region_under_cursor = area ? ED_area_find_region_xy_visual(
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

  /* Commit and tear down when the user switches to another tool. Switching the active tool does
   * not cancel a running modal, so without this the gradient's modal -- and its handle overlay --
   * would keep floating over the newly selected tool. This also runs on the 30 FPS timer, so it
   * fires shortly after a switch even when the pointer is stationary. */
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
    WM_cursor_modal_set(CTX_wm_window(C), show_crosshair ? WM_CURSOR_CROSS : WM_CURSOR_DEFAULT);
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
    if (ELEM(event->val, IMAGE_SELECT_FLOATING_MODAL_CANCEL, IMAGE_SELECT_FLOATING_MODAL_CONFIRM))
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

  const bool is_curve = image_select_gradient_is_curve(CTX_data_scene(C));

  /* Curve hover feedback before event routing: idle mouse moves pass through below, but the
   * overlay still needs to know which control point is under the mouse. */
  if (is_curve && event->type == MOUSEMOVE && data->drag_mode == GradientDragMode::Idle) {
    const GradientCurveHitResult hover_hit = image_select_gradient_curve_hit_test(
        region, event, state);
    const int hover = (hover_hit.type == GradientCurveHitType::ControlPoint) ?
                          hover_hit.point_index :
                          -1;
    if (hover != state->curve_hover_point) {
      state->curve_hover_point = hover;
      ED_region_tag_redraw(region);
    }
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
    if (is_curve) {
      data->drag_mode = image_select_gradient_curve_press_resolve(
          region, ima, event, CTX_data_scene(C), state, data);
      /* A press can already have changed the curve (insert, new-curve start, selection). */
      image_select_gradient_update_preview(C, state, false);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (image_select_gradient_hit_test_handle(region, event, state->start_uv)) {
      data->drag_mode = GradientDragMode::Start;
    }
    else if (image_select_gradient_hit_test_handle(region, event, state->end_uv)) {
      data->drag_mode = GradientDragMode::End;
    }
    else if (image_select_gradient_hit_test_handle(
                 region, event, image_select_gradient_mid_uv_for_type(state, is_curve)))
    {
      data->drag_mode = GradientDragMode::Mid;
    }
    else {
      /* Click on empty space: start a brand-new gradient from this point. */
      data->drag_mode = GradientDragMode::New;
      int tile_number = 0;
      float2 uv;
      if (region && image_select_gradient_mouse_to_global_uv(region, ima, event, tile_number, uv))
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
            if (is_curve && state->curve != nullptr) {
              const float2 mid_px = image_select_gradient_curve_px_from_uv(state, uv);
              const gradient_curve::CurveProjection proj = state->curve->project(mid_px);
              state->midpoint = clamp_f(proj.s / state->curve->length(), 0.05f, 0.95f);
            }
            else {
              state->midpoint = image_select_gradient_project_midpoint(
                  uv, state->start_uv, state->end_uv);
            }
            break;
          case GradientDragMode::End:
            state->end_uv = uv;
            break;
          case GradientDragMode::PointDrag: {
            /* Move the dragged selection rigidly: each point takes its pre-drag position plus
             * the total drag delta. */
            const float2 delta = uv - data->drag_press_uv;
            for (const int point_index : data->drag_points) {
              if (point_index >= 0 && point_index < state->curve_points_uv.size()) {
                state->curve_points_uv[point_index] = data->drag_orig_points[point_index] + delta;
              }
            }
            image_select_gradient_curve_rebuild(state, CTX_data_scene(C)->toolsettings->imapaint);
            break;
          }
          case GradientDragMode::New:
            if (is_curve) {
              /* Collect surface points while the button is held; the threshold is in region
               * pixels so slow strokes do not crowd the curve. */
              float rx, ry;
              ui::view2d_view_to_region_fl(&region->v2d, uv.x, uv.y, &rx, &ry);
              const float2 cur_px(rx, ry);
              if (len_v2v2(cur_px, data->last_collect_px) >= GRADIENT_CURVE_COLLECT_DIST_PX)
              {
                state->curve_points_uv.append(uv);
                data->last_collect_px = cur_px;
                image_select_gradient_curve_rebuild(
                    state, CTX_data_scene(C)->toolsettings->imapaint);
              }
              state->end_uv = uv;
            }
            else {
              state->end_uv = uv;
            }
            break;
          case GradientDragMode::Idle:
          default:
            state->end_uv = uv;
            break;
        }
        image_select_gradient_update_preview(C, state, event->type == LEFTMOUSE);
      }
    }

    if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
      const GradientDragMode released_mode = data->drag_mode;
      /* The stroke is over: reveal the control points again (before the simplify pass below, so
       * the redraw shows the settled points). Tag a redraw explicitly because the throttled
       * preview update below may skip one. */
      state->curve_is_drawing = false;
      if (region) {
        ED_region_tag_redraw(region);
      }
      /* End the active drag; transition to Idle so MOUSEMOVE no longer moves handles. */
      data->drag_mode = GradientDragMode::Idle;
      /* A finished draw gesture is the user's chance to settle the curve: thin the raw samples
       * into a handful of editable control points, then rebuild and re-sync so the redo panel and
       * the overlay agree on the simplified form. */
      if (is_curve && released_mode == GradientDragMode::New && state->curve_points_uv.size() > 2) {
        image_select_gradient_curve_simplify(state, CTX_data_scene(C)->toolsettings->imapaint);
        image_select_gradient_update_preview(C, state, true);
      }
      /* Mirror the drawn curve into the operator's RNA for the redo panel / scripting. */
      image_select_gradient_sync_curve_to_op(op, state);
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

  /* Curve editing keys: Delete / X remove the selected control points, A toggles select-all.
   * The floating curve owns these keys only while a session is up, and only for its own
   * redraw -- everything else passes through. */
  if (is_curve && event->val == KM_PRESS) {
    if (ELEM(event->type, EVT_DELKEY, EVT_XKEY) && !state->curve_selected_points.is_empty()) {
      image_select_gradient_curve_remove_selected(state,
                                                  CTX_data_scene(C)->toolsettings->imapaint);
      image_select_gradient_update_preview(C, state, true);
      image_select_gradient_sync_curve_to_op(op, state);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == EVT_AKEY && !state->curve_points_uv.is_empty()) {
      Vector<int> &selected = state->curve_selected_points;
      if (selected.size() == state->curve_points_uv.size()) {
        selected.clear();
      }
      else {
        selected.reinitialize(state->curve_points_uv.size());
        for (const int i : selected.index_range()) {
          selected[i] = i;
        }
      }
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
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
      {IMAGE_PAINT_GRADIENT_CURVE, "CURVE", 0, "Curve", ""},
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
  /* Hidden mirror of the floating session's drawn curve, one global UV per element (filled on
   * drag release, see #image_select_gradient_sync_curve_to_op). */
  prop = RNA_def_collection_runtime(
      ot->srna, "curve_points", RNA_OperatorMousePath, "Curve Points", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  RNA_def_property_ui_text(prop,
                           "Curve Points",
                           "Global UV points of the drawn gradient curve, in stroke order");
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
