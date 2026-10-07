/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Quick Selection for Image Paint: a brush whose selection "spreads" from the stroke over
 * similar pixels until it hits a contrasted edge or a UV seam.
 *
 * Per stroke: the feature image, UV-island map and Sobel edge map of the source tile are built
 * once. Each dab grows a geodesic region from the brush center with per-pixel step costs
 * `1 + alpha * edge + beta * dissimilarity` inside a budget of `spread * radius` steps, so the
 * front slows down on edges and at color changes and the budget stops it. The grown dab merges
 * into the selection (Add) or is carved out of it (Subtract, growing only through selected
 * pixels).
 *
 * The stroke edits a private working mask at the source resolution. The preview copies the dab
 * window into one target; on release every target is rewritten from its snapshot and receives
 * the stroke's *delta* (the working mask against its stroke-start state, resampled to the
 * target's resolution). One image undo step per stroke.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "UI_view2d.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "paint_image_select_gesture.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_region.hh"
#include "paint_image_select_source.hh"

namespace blender {

namespace image_select = ed::sculpt_paint::image_select;

namespace {

/**
 * Foreground / background feature model of the Quick Selection: two sparse histograms over the
 * quantized feature space (16^3 bins for color, OkLab and normals, 8^4 for RGBA, 64 for scalars).
 * The score of a pixel is the foreground share of its bin, `p_fg / (p_fg + p_bg)`, where the
 * counts are normalized by the size of each sample set. A bin nobody has sampled looks at its
 * direct neighbors, and stays "unknown" when they are empty too, so the caller only trusts the
 * model where the strokes have actually seen the color.
 */
struct ColorModel {
  int dims = 0;
  int bins_per_dim = 0;
  /** Features are in -1..1 (OkLab, normals) instead of 0..1. */
  bool is_signed = false;
  Array<float> fg;
  Array<float> bg;
  float fg_total = 0.0f;
  float bg_total = 0.0f;

  void init(const int feature_dims, const bool signed_range)
  {
    this->dims = feature_dims;
    this->is_signed = signed_range;
    this->bins_per_dim = (feature_dims == 1) ? 64 : ((feature_dims == 4) ? 8 : 16);
    int64_t bin_num = 1;
    for (int i = 0; i < feature_dims; i++) {
      bin_num *= this->bins_per_dim;
    }
    this->fg.reinitialize(bin_num);
    this->fg.fill(0.0f);
    this->bg.reinitialize(bin_num);
    this->bg.fill(0.0f);
    this->fg_total = 0.0f;
    this->bg_total = 0.0f;
  }

  bool is_valid() const
  {
    return this->dims > 0;
  }

  /** Per-dimension bin coordinates of \a value. */
  void coords_of(const float *value, int r_coords[4]) const
  {
    for (int c = 0; c < this->dims; c++) {
      const float unit = this->is_signed ? (value[c] + 1.0f) * 0.5f : value[c];
      r_coords[c] = std::clamp(int(unit * float(this->bins_per_dim)), 0, this->bins_per_dim - 1);
    }
  }

  int64_t index_of(const int coords[4]) const
  {
    int64_t index = 0;
    for (int c = 0; c < this->dims; c++) {
      index = index * this->bins_per_dim + coords[c];
    }
    return index;
  }

  void add_fg(const float *value)
  {
    int coords[4];
    this->coords_of(value, coords);
    this->fg[this->index_of(coords)] += 1.0f;
    this->fg_total += 1.0f;
  }

  void add_bg(const float *value)
  {
    int coords[4];
    this->coords_of(value, coords);
    this->bg[this->index_of(coords)] += 1.0f;
    this->bg_total += 1.0f;
  }

  /**
   * Foreground share of \a value in 0..1. \a r_known is false when neither set has sampled the
   * bin or its direct neighbors (the score is then 0.5). Needs samples in both sets.
   */
  float score(const float *value, bool &r_known) const
  {
    r_known = false;
    if (this->fg_total <= 0.0f || this->bg_total <= 0.0f) {
      return 0.5f;
    }
    int coords[4];
    this->coords_of(value, coords);
    float fg_count = this->fg[this->index_of(coords)];
    float bg_count = this->bg[this->index_of(coords)];
    if (fg_count + bg_count <= 0.0f) {
      /* Direct neighbors along each axis. */
      for (int c = 0; c < this->dims; c++) {
        for (const int step : {-1, 1}) {
          const int moved = coords[c] + step;
          if (moved < 0 || moved >= this->bins_per_dim) {
            continue;
          }
          const int saved = coords[c];
          coords[c] = moved;
          const int64_t neighbor = this->index_of(coords);
          coords[c] = saved;
          fg_count += this->fg[neighbor];
          bg_count += this->bg[neighbor];
        }
      }
      if (fg_count + bg_count <= 0.0f) {
        return 0.5f;
      }
    }
    r_known = true;
    const float p_fg = fg_count / this->fg_total;
    const float p_bg = bg_count / this->bg_total;
    return p_fg / (p_fg + p_bg);
  }
};

/** Largest brush radius in image pixels: bounds the per-dab window (and so the dab cost) when the
 * view is zoomed far out. */
constexpr int QUICK_RADIUS_MAX = 128;
/** Largest half size of the per-dab window in image pixels. */
constexpr int QUICK_WINDOW_MAX = 640;
/** Model score below which a known color is treated as the other side of the selection. */
constexpr float QUICK_MODEL_WALL = 0.15f;
/** Half width in image pixels of the boundary band that Auto-Enhance re-decides by the model. */
constexpr int QUICK_ENHANCE_BAND = 3;
/** Cap of the background samples taken at the stroke start. */
constexpr int QUICK_BG_SAMPLE_MAX = 20000;
/** Band around the existing selection (image pixels) the background is sampled from. */
constexpr int QUICK_BG_BAND_MIN = 8;
constexpr int QUICK_BG_BAND_MAX = 16;

/* -------------------------------------------------------------------- */
/** \name Stroke state
 * \{ */

struct QuickStroke {

  /* Source (per stroke). */
  image_select::SelectSource source;
  image_select::FeatureImage feature;
  image_select::UVIslandMap barrier;
  Array<float> edge;
  int2 size = int2(0, 0);

  /* Foreground model: running average of the dab samples. */
  Array<float> fg_sample;
  int fg_count = 0;
  /** Foreground/background histograms: the brush is foreground, the band around the selection (or
   * the kept pixels, when subtracting) is background. */
  ColorModel model;

  /* Parameters. */
  bool subtract = false;
  bool auto_enhance = true;
  float spread = 4.0f;
  float edge_sensitivity = 0.5f;
  float radius_screen = 25.0f;
  int radius_image = 25;
  float feather_px = 0.0f;

  /* Stroke progress. */
  bool have_last_dab = false;
  float2 last_dab_uv = float2(0.0f);
  int2 last_mval = int2(0);
  /** Union of the dab windows written so far (image pixels, max exclusive). */
  int2 dirty_min = int2(0, 0);
  int2 dirty_max = int2(0, 0);

  /** Selection state of the stroke at the source resolution, and its stroke-start copy. */
  Array<float> work;
  Array<float> work_orig;

  Vector<ImagePaintSelectionTarget> targets;
  Vector<ImagePaintSelectionMaskSnapshot> snapshots;
  /** The snapshot whose target shows the live preview. */
  int preview_snapshot = 0;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke setup and teardown
 * \{ */

/**
 * Snapshot the targets and seed the working mask from the preview target (the source image's own
 * mask when it is a target, else the first one), resampled to the source resolution.
 */
static bool quick_stroke_work_init(QuickStroke &stroke)
{
  stroke.snapshots = image_paint_selection_mask_snapshots_create(stroke.targets,
                                                                 stroke.source.tile_number);
  if (stroke.snapshots.is_empty()) {
    return false;
  }
  stroke.preview_snapshot = 0;
  for (const int i : stroke.snapshots.index_range()) {
    if (stroke.snapshots[i].image == stroke.source.image) {
      stroke.preview_snapshot = i;
      break;
    }
  }
  const ImBuf *ref = stroke.snapshots[stroke.preview_snapshot].orig;
  const int2 ref_size(ref->x, ref->y);
  const Span<float> ref_data(ref->float_data(), int64_t(ref_size.x) * ref_size.y);

  stroke.work.reinitialize(int64_t(stroke.size.x) * stroke.size.y);
  if (ref_size == stroke.size) {
    stroke.work.as_mutable_span().copy_from(ref_data);
  }
  else {
    image_select::mask_resample(ref_data, ref_size, stroke.work, stroke.size);
  }
  stroke.work_orig = stroke.work;
  return true;
}

/** Copy the working mask's \a min..\a max window into the preview target's live mask. */
static void quick_preview_write(QuickStroke &stroke, const int2 min_px, const int2 max_px)
{
  const ImagePaintSelectionMaskSnapshot &snapshot = stroke.snapshots[stroke.preview_snapshot];
  ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(snapshot.image, snapshot.tile_number);
  if (!tile_mask || !tile_mask->float_data()) {
    return;
  }
  const int2 dst_size(tile_mask->x, tile_mask->y);
  float *dst = tile_mask->float_data_for_write();
  if (dst_size == stroke.size) {
    for (const int y : IndexRange(min_px.y, max_px.y - min_px.y)) {
      const int64_t offset = int64_t(y) * stroke.size.x + min_px.x;
      std::memcpy(dst + offset, &stroke.work[offset], sizeof(float) * (max_px.x - min_px.x));
    }
    return;
  }
  /* A preview target of another resolution than the source: resample the whole mask. */
  Array<float> resampled(int64_t(dst_size.x) * dst_size.y);
  image_select::mask_resample(stroke.work, stroke.size, resampled, dst_size);
  std::memcpy(dst, resampled.data(), sizeof(float) * resampled.size());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dabs
 * \{ */

/** Running FG sample updated with the dab's brush-square mean. */
static void quick_fg_sample_update(QuickStroke &stroke, const int2 &center)
{
  Array<float> mean(stroke.feature.dims);
  image_select::feature_sample_window(stroke.feature, center, stroke.radius_image * 2 + 1, mean);
  const float weight = 1.0f / float(stroke.fg_count + 1);
  for (const int c : IndexRange(stroke.feature.dims)) {
    stroke.fg_sample[c] = math::interpolate(stroke.fg_sample[c], mean[c], weight);
  }
  stroke.fg_count++;
  /* The brush disc is foreground evidence; a stride keeps a big brush at about 1000 samples. */
  const int radius = stroke.radius_image;
  const int stride = std::max(1, radius / 16);
  for (int dy = -radius; dy <= radius; dy += stride) {
    for (int dx = -radius; dx <= radius; dx += stride) {
      const int x = center.x + dx, y = center.y + dy;
      if (dx * dx + dy * dy > radius * radius || x < 0 || y < 0 || x >= stroke.size.x ||
          y >= stroke.size.y)
      {
        continue;
      }
      stroke.model.add_fg(stroke.feature.pixel(x, y));
    }
  }
}

static void quick_dab(QuickStroke &stroke, const float2 &dab_uv)
{
  if (stroke.feature.is_empty()) {
    return;
  }
  const float2 origin = image_select_udim_tile_uv_origin(stroke.source.tile_number);
  const float2 center_px = (dab_uv - origin) * float2(stroke.size);
  const int2 center = int2(int(std::floor(center_px.x)), int(std::floor(center_px.y)));
  if (center.x < 0 || center.y < 0 || center.x >= stroke.size.x || center.y >= stroke.size.y) {
    /* The dab center is off the tile. */
    return;
  }

  /* Window around the dab. */
  const int budget_px = int(stroke.spread * float(stroke.radius_image));
  const int win = std::min(budget_px + stroke.radius_image + 2, QUICK_WINDOW_MAX);
  const int2 window_min(std::max(0, center.x - win), std::max(0, center.y - win));
  const int2 window_max(std::min(stroke.size.x, center.x + win + 1),
                        std::min(stroke.size.y, center.y + win + 1));
  const int ww = window_max.x - window_min.x;
  const int wh = window_max.y - window_min.y;
  const int64_t wn = int64_t(ww) * wh;

  const float alpha = stroke.edge_sensitivity * 40.0f;
  const float beta = 20.0f;
  const float gamma = 20.0f;
  const int64_t size_x = stroke.size.x;
  const float *work = stroke.work.data();

  /* Lazy per-window cache of the model score (NaN = not computed yet), shared by the step cost
   * and the wall test so a pixel is never scored twice. */
  Array<float> score_cache(wn, std::numeric_limits<float>::quiet_NaN());
  Array<uint8_t> known_cache(wn, 0);
  const auto model_score_of = [&](const int gx, const int gy, bool &r_known) {
    const int64_t li = int64_t(gy - window_min.y) * ww + (gx - window_min.x);
    if (!std::isnan(score_cache[li])) {
      r_known = known_cache[li] != 0;
      return score_cache[li];
    }
    bool known = false;
    const float score = stroke.model.score(stroke.feature.pixel(gx, gy), known);
    score_cache[li] = score;
    known_cache[li] = known;
    r_known = known;
    return score;
  };

  /* Step cost of entering a pixel: its distance to the FG model and the local edge strength. */
  const auto step_cost = [&](const int2 local) {
    const int gx = window_min.x + local.x, gy = window_min.y + local.y;
    const float *pixel = stroke.feature.pixel(gx, gy);
    const float dist = image_select::feature_distance(
        stroke.feature, stroke.fg_sample.data(), pixel);
    const float dissimilar = math::clamp(dist / 0.5f, 0.0f, 1.0f);
    bool known = false;
    const float score = model_score_of(gx, gy, known);
    const float model_cost = known ? gamma * (1.0f - score) : 0.0f;
    return 1.0f + alpha * stroke.edge[int64_t(gy) * size_x + gx] + beta * dissimilar + model_cost;
  };
  /* Barrier: island borders, and (Subtract) unselected pixels. */
  const auto passable = [&](const int2 from, const int2 to) {
    const int2 gfrom = window_min + from;
    const int2 gto = window_min + to;
    if (stroke.barrier.is_valid() && !stroke.barrier.passable(gfrom, gto)) {
      return false;
    }
    if (stroke.subtract && work[int64_t(gto.y) * size_x + gto.x] < 0.5f) {
      return false;
    }
    /* A color the model has clearly seen on the other side (background) is a wall. */
    bool known = false;
    const float score = model_score_of(gto.x, gto.y, known);
    if (known && score < QUICK_MODEL_WALL) {
      return false;
    }
    return true;
  };

  Array<float> cost(wn);
  image_select::geodesic_cost_map(step_cost,
                                  int2(ww, wh),
                                  center - window_min,
                                  true,
                                  float(std::min(budget_px, QUICK_WINDOW_MAX)),
                                  passable,
                                  cost);

  /* Accepted pixels + the brush disc. */
  Array<float> fg(wn, 0.0f);
  const int2 seed_local = center - window_min;
  const float radius_sq = float(stroke.radius_image) * float(stroke.radius_image);
  for (const int ly : IndexRange(wh)) {
    for (const int lx : IndexRange(ww)) {
      const int64_t li = int64_t(ly) * ww + lx;
      const float dx = float(lx - seed_local.x), dy = float(ly - seed_local.y);
      const bool in_disc = (dx * dx + dy * dy) <= radius_sq;
      fg[li] = (in_disc || cost[li] != std::numeric_limits<float>::max()) ? 1.0f : 0.0f;
    }
  }

  /* Auto-enhance: smooth the dab boundary inside the window, then re-threshold. */
  if (stroke.auto_enhance) {
    Array<float> smooth(wn);
    image_select::gaussian_blur_barrier(fg, int2(ww, wh), 1.0f, nullptr, smooth);
    for (const int64_t li : IndexRange(wn)) {
      fg[li] = (smooth[li] >= 0.5f) ? 1.0f : 0.0f;
    }
    /* Snap the boundary to the model: inside a band of QUICK_ENHANCE_BAND px around the dab edge
     * a pixel joins when the model rates it foreground, and leaves when it rates it background.
     * Pixels the model has not seen keep the smoothed result. The brush disc and pixels behind a
     * seam (or, when subtracting, outside the selection) are never changed. */
    if (stroke.model.fg_total > 0.0f && stroke.model.bg_total > 0.0f) {
      const int band = QUICK_ENHANCE_BAND;
      Array<float> dilated(fg);
      Array<float> eroded(fg);
      for (const int pass : IndexRange(2)) {
        /* Separable max / min filter: rows on the first pass, columns on the second. */
        const Array<float> dil_src(dilated);
        const Array<float> ero_src(eroded);
        for (const int ly : IndexRange(wh)) {
          for (const int lx : IndexRange(ww)) {
            float hi = 0.0f, lo = 1.0f;
            for (int k = -band; k <= band; k++) {
              const int sx = (pass == 0) ? lx + k : lx;
              const int sy = (pass == 0) ? ly : ly + k;
              if (sx < 0 || sy < 0 || sx >= ww || sy >= wh) {
                continue;
              }
              const int64_t si = int64_t(sy) * ww + sx;
              hi = std::max(hi, dil_src[si]);
              lo = std::min(lo, ero_src[si]);
            }
            const int64_t li = int64_t(ly) * ww + lx;
            dilated[li] = hi;
            eroded[li] = lo;
          }
        }
      }
      const int32_t seed_island = stroke.barrier.is_valid() ? stroke.barrier.island_at(center) :
                                                              -1;
      for (const int ly : IndexRange(wh)) {
        for (const int lx : IndexRange(ww)) {
          const int64_t li = int64_t(ly) * ww + lx;
          if (!(dilated[li] > 0.5f && eroded[li] < 0.5f)) {
            continue;
          }
          const float dx = float(lx - seed_local.x), dy = float(ly - seed_local.y);
          if (dx * dx + dy * dy <= radius_sq) {
            continue;
          }
          const int gx = window_min.x + lx, gy = window_min.y + ly;
          if (stroke.barrier.is_valid() && !stroke.barrier.overlap_at(int2(gx, gy)) &&
              stroke.barrier.island_at(int2(gx, gy)) != seed_island)
          {
            continue;
          }
          if (stroke.subtract && work[int64_t(gy) * size_x + gx] < 0.5f) {
            continue;
          }
          bool known = false;
          const float score = stroke.model.score(stroke.feature.pixel(gx, gy), known);
          if (known) {
            fg[li] = (score > 0.5f) ? 1.0f : 0.0f;
          }
        }
      }
    }
  }

  quick_fg_sample_update(stroke, center);

  /* Blend the dab into the working mask by the effective mode. */
  float *work_write = stroke.work.data();
  for (const int ly : IndexRange(wh)) {
    for (const int lx : IndexRange(ww)) {
      const float value = fg[int64_t(ly) * ww + lx];
      float &pixel = work_write[int64_t(window_min.y + ly) * size_x + window_min.x + lx];
      pixel = stroke.subtract ? std::min(pixel, 1.0f - value) : std::max(pixel, value);
    }
  }

  if (stroke.dirty_max.x <= stroke.dirty_min.x) {
    stroke.dirty_min = window_min;
    stroke.dirty_max = window_max;
  }
  else {
    stroke.dirty_min = math::min(stroke.dirty_min, window_min);
    stroke.dirty_max = math::max(stroke.dirty_max, window_max);
  }
  quick_preview_write(stroke, window_min, window_max);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush cursor
 * \{ */
/** Only while the Quick Select tool is the active tool of a paint-mode Image Editor. */
static bool quick_cursor_poll(bContext *C)
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || sima->mode != SI_MODE_PAINT) {
    return false;
  }
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  return tref && STREQ(tref->idname, "builtin.select_quick");
}

/**
 * Brush ring with a `+` (Add) or `-` (Subtract) sign, so the mode is readable at a glance. Alt
 * flips the mode for a stroke, so it flips the sign too.
 */
static void quick_cursor_draw(bContext *C,
                              const int2 &xy,
                              const float2 & /*tilt*/,
                              void * /*customdata*/)
{
  const Scene *scene = CTX_data_scene(C);
  if (!scene || !scene->toolsettings) {
    return;
  }
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  const wmWindow *win = CTX_wm_window(C);
  const bool alt = win && win->runtime && win->runtime->eventstate &&
                   (win->runtime->eventstate->modifier & KM_ALT) != 0;
  const bool subtract = (imapaint.quick_select_mode == IMAGE_PAINT_QUICK_SELECT_SUBTRACT) != alt;
  const float radius = imapaint.quick_select_radius;
  const float x = float(xy.x), y = float(xy.y);

  GPU_line_smooth(true);
  GPU_blend(GPU_BLEND_ALPHA);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* Ring: dark underlay, light line. */
  GPU_line_width(3.0f);
  immUniformColor4f(0.0f, 0.0f, 0.0f, 0.6f);
  imm_draw_circle_wire_2d(pos, x, y, radius, 48);
  GPU_line_width(1.0f);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
  imm_draw_circle_wire_2d(pos, x, y, radius, 48);

  /* Sign at the ring center; green for Add, red for Subtract. */
  const float arm = std::clamp(radius * 0.25f, 4.0f, 10.0f);
  const float3 color = subtract ? float3(1.0f, 0.3f, 0.3f) : float3(0.4f, 1.0f, 0.4f);
  for (const float width : {4.0f, 2.0f}) {
    GPU_line_width(width);
    if (width > 3.0f) {
      immUniformColor4f(0.0f, 0.0f, 0.0f, 0.7f);
    }
    else {
      immUniformColor4f(color.x, color.y, color.z, 1.0f);
    }
    immBegin(GPU_PRIM_LINES, subtract ? 2 : 4);
    immVertex2f(pos, x - arm, y);
    immVertex2f(pos, x + arm, y);
    if (!subtract) {
      immVertex2f(pos, x, y - arm);
      immVertex2f(pos, x, y + arm);
    }
    immEnd();
  }

  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static bool image_select_quick_poll(bContext *C)
{
  return image_paint_selection_image_poll(C);
}

static float2 quick_region_to_uv(const ARegion *region, const int2 &mval)
{
  float uv[2];
  ui::view2d_region_to_view(&region->v2d, float(mval.x), float(mval.y), &uv[0], &uv[1]);
  return float2(uv[0], uv[1]);
}

static void quick_dabs_interpolated(QuickStroke &stroke, const float2 &to_uv)
{
  if (!stroke.have_last_dab) {
    quick_dab(stroke, to_uv);
    stroke.last_dab_uv = to_uv;
    stroke.have_last_dab = true;
    return;
  }
  /* Dab spacing: a quarter of the brush radius. */
  const float length_px = math::length((to_uv - stroke.last_dab_uv) * float2(stroke.size));
  const float spacing_px = std::max(0.25f * float(stroke.radius_image), 1.0f);
  const int steps = std::min(int(length_px / spacing_px), 64);
  for (const int i : IndexRange(1, steps)) {
    const float t = float(i) / float(steps + 1);
    quick_dab(stroke, stroke.last_dab_uv + (to_uv - stroke.last_dab_uv) * t);
  }
  quick_dab(stroke, to_uv);
  stroke.last_dab_uv = to_uv;
}

static void quick_stroke_free(QuickStroke *stroke)
{
  image_paint_selection_mask_snapshots_free(stroke->snapshots);
  MEM_delete(stroke);
}

/** Rewrite every target from the stroke's delta and close the undo step. */
static void quick_stroke_commit(bContext *C, QuickStroke *stroke)
{
  const int64_t src_pixel_num = int64_t(stroke->size.x) * stroke->size.y;

  /* The stroke's delta: coverage added (Add) or removed (Subtract) against the stroke start. */
  Array<float> delta(src_pixel_num);
  const bool subtract = stroke->subtract;
  threading::parallel_for(IndexRange(src_pixel_num), 4096, [&](const IndexRange range) {
    for (const int64_t i : range) {
      delta[i] = subtract ? std::max(0.0f, stroke->work_orig[i] - stroke->work[i]) :
                            std::max(0.0f, stroke->work[i] - stroke->work_orig[i]);
    }
  });

  image_paint_selection_mask_snapshots_restore(stroke->snapshots);
  for (const ImagePaintSelectionMaskSnapshot &snapshot : stroke->snapshots) {
    ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(snapshot.image, snapshot.tile_number);
    if (!tile_mask || !tile_mask->float_data()) {
      continue;
    }
    const int2 dst_size(tile_mask->x, tile_mask->y);
    const MutableSpan<float> dst(tile_mask->float_data_for_write(),
                                 int64_t(dst_size.x) * dst_size.y);
    const eSelectOp op = subtract ? SEL_OP_SUB : SEL_OP_ADD;
    if (dst_size == stroke->size) {
      image_select::mask_combine(dst, delta, op);
    }
    else {
      Array<float> resampled(dst.size());
      image_select::mask_resample(delta, stroke->size, resampled, dst_size);
      image_select::mask_combine(dst, resampled, op);
    }
  }

  /* Face/Island expansion of what the stroke added; a removal keeps its pixel result. */
  if (!subtract && stroke->dirty_max.x > stroke->dirty_min.x) {
    const float2 origin = image_select_udim_tile_uv_origin(stroke->source.tile_number);
    rctf bounds;
    BLI_rctf_init_minmax(&bounds);
    BLI_rctf_do_minmax_v(&bounds, origin + float2(stroke->dirty_min) / float2(stroke->size));
    BLI_rctf_do_minmax_v(&bounds, origin + float2(stroke->dirty_max) / float2(stroke->size));
    for (const ImagePaintSelectionTarget &target : stroke->targets) {
      image_paint_selection_expand(C, target.image, SEL_OP_ADD, &bounds);
    }
  }

  image_paint_selection_edge_policy_apply(stroke->targets, stroke->feather_px);
  image_paint_selection_targets_update(C, stroke->targets);
  ED_image_undo_push_end();
}

static void quick_stroke_cancel(bContext *C, QuickStroke *stroke)
{
  image_paint_selection_mask_snapshots_restore_for_cancel(stroke->snapshots);
  image_paint_selection_targets_update(C, stroke->targets);
  ED_image_undo_push_end();
}

/**
 * Seed the background model. Adding: the unselected band 8..16 px outside the current selection
 * (Euclidean distance transform), or every unselected pixel when nothing is selected yet or the
 * band is tiny. Subtracting: the selected pixels, which are the ones to keep. Restricted to the
 * island under the press when UV Borders is on, thinned to at most #QUICK_BG_SAMPLE_MAX samples.
 */
static void quick_model_init(QuickStroke &stroke, const int2 &press_px)
{
  stroke.model.init(stroke.feature.dims,
                    stroke.feature.is_normal ||
                        stroke.feature.metric == image_select::SourceMetric::OkLab);

  const int64_t pixel_num = int64_t(stroke.size.x) * stroke.size.y;
  const bool use_island = stroke.barrier.is_valid();
  const int32_t press_island = use_island ? stroke.barrier.island_at(press_px) : -1;
  const auto in_island = [&](const int64_t i) {
    return !use_island || stroke.barrier.island[i] == press_island || stroke.barrier.overlap[i];
  };
  const auto selected = [&](const int64_t i) { return stroke.work_orig[i] >= 0.5f; };

  /* Eligible pixels: a per-pixel predicate, evaluated twice (count, then take). */
  Array<float> band_dist;
  bool use_band = false;
  if (!stroke.subtract) {
    Array<uint8_t> binary(pixel_num);
    bool any_selected = false;
    for (const int64_t i : IndexRange(pixel_num)) {
      binary[i] = selected(i);
      any_selected = any_selected || binary[i];
    }
    if (any_selected) {
      band_dist.reinitialize(pixel_num);
      image_select::distance_transform_true(binary, stroke.size, band_dist);
      use_band = true;
    }
  }
  const auto eligible_band = [&](const int64_t i) {
    return in_island(i) && !selected(i) && band_dist[i] >= float(QUICK_BG_BAND_MIN) &&
           band_dist[i] <= float(QUICK_BG_BAND_MAX);
  };
  const auto eligible_plain = [&](const int64_t i) {
    return in_island(i) && (stroke.subtract ? selected(i) : !selected(i));
  };

  const auto count_of = [&](const auto &predicate) {
    int64_t count = 0;
    for (const int64_t i : IndexRange(pixel_num)) {
      count += predicate(i) ? 1 : 0;
    }
    return count;
  };
  int64_t count = use_band ? count_of(eligible_band) : 0;
  const bool band_used = use_band && count >= 500;
  if (!band_used) {
    count = count_of(eligible_plain);
  }
  if (count == 0) {
    return;
  }
  const int64_t stride = std::max<int64_t>(1, count / QUICK_BG_SAMPLE_MAX);
  int64_t seen = 0;
  for (const int64_t i : IndexRange(pixel_num)) {
    if (band_used ? !eligible_band(i) : !eligible_plain(i)) {
      continue;
    }
    if ((seen++ % stride) == 0) {
      stroke.model.add_bg(&stroke.feature.data[i * stroke.feature.dims]);
    }
  }
}

static wmOperatorStatus image_select_quick_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  ARegion *region = CTX_wm_region(C);
  const Scene *scene = CTX_data_scene(C);
  if (!sima || !region || !scene || !scene->toolsettings) {
    return OPERATOR_CANCELLED;
  }
  image_select_floating_sessions_end_all(C, sima);

  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  const int2 press_mval(event->mval[0], event->mval[1]);
  const float2 press_uv = quick_region_to_uv(region, press_mval);

  auto *stroke = MEM_new<QuickStroke>(__func__);
  stroke->targets = image_paint_selection_targets_get(C, sima);
  if (stroke->targets.is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "Quick Select: no target images to select on");
    MEM_delete(stroke);
    return OPERATOR_CANCELLED;
  }
  if (!image_select::select_source_resolve_or_report(C, op->reports, press_uv, stroke->source)) {
    MEM_delete(stroke);
    return OPERATOR_CANCELLED;
  }

  stroke->feature = image_select::feature_image_build(
      stroke->source.image,
      &stroke->source.iuser,
      stroke->source.tile_number,
      image_select::SourceComponent(imapaint.select_component),
      image_select::SourceMetric(imapaint.select_metric),
      stroke->source.is_normal,
      (imapaint.select_flag & IMAGE_PAINT_SELECT_NORMALIZE_RANGE) != 0);
  if (stroke->feature.is_empty()) {
    MEM_delete(stroke);
    return OPERATOR_CANCELLED;
  }
  stroke->size = stroke->feature.size;
  if ((imapaint.select_flag & IMAGE_PAINT_SELECT_USE_UV_BOUNDS) != 0) {
    stroke->barrier = image_select::uv_island_map_build(C,
                                                        stroke->source.image,
                                                        stroke->source.tile_number,
                                                        stroke->size.x,
                                                        stroke->size.y,
                                                        imapaint.select_uv_margin_px);
  }
  stroke->edge.reinitialize(int64_t(stroke->size.x) * stroke->size.y);
  image_select::sobel_edge_map(
      stroke->feature.data, stroke->feature.dims, stroke->size, stroke->edge);

  stroke->subtract = ((event->modifier & KM_ALT) != 0) ||
                     (imapaint.quick_select_mode == IMAGE_PAINT_QUICK_SELECT_SUBTRACT);
  stroke->auto_enhance = (imapaint.quick_select_flag & IMAGE_PAINT_QUICK_SELECT_AUTO_ENHANCE) != 0;
  stroke->spread = imapaint.quick_select_spread;
  stroke->edge_sensitivity = imapaint.quick_select_edge_sensitivity;
  stroke->radius_screen = imapaint.quick_select_radius;
  stroke->feather_px = imapaint.select_feather_px;

  /* Screen radius to image pixels: how many UV units a radius covers at the current zoom. */
  const float2 edge_uv = quick_region_to_uv(region,
                                            press_mval + int2(int(stroke->radius_screen), 0));
  const float radius_uv = std::abs(edge_uv.x - press_uv.x);
  stroke->radius_image = std::clamp(int(radius_uv * float(stroke->size.x)), 1, QUICK_RADIUS_MAX);

  image_paint_selection_undo_begin("Quick Select", stroke->targets);
  if (!quick_stroke_work_init(*stroke)) {
    ED_image_undo_push_end();
    MEM_delete(stroke);
    return OPERATOR_CANCELLED;
  }

  stroke->fg_sample.reinitialize(stroke->feature.dims);
  stroke->fg_sample.fill(0.0f);
  const float2 origin = image_select_udim_tile_uv_origin(stroke->source.tile_number);
  const float2 seed_px_f = (press_uv - origin) * float2(stroke->size);
  const int2 seed_px(std::clamp(int(std::floor(seed_px_f.x)), 0, stroke->size.x - 1),
                     std::clamp(int(std::floor(seed_px_f.y)), 0, stroke->size.y - 1));
  quick_model_init(*stroke, seed_px);
  quick_fg_sample_update(*stroke, seed_px);

  /* First dab. */
  stroke->last_mval = press_mval;
  quick_dabs_interpolated(*stroke, press_uv);

  op->customdata = stroke;
  WM_event_add_modal_handler(C, op);
  image_paint_selection_targets_update(C, stroke->targets);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus image_select_quick_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  QuickStroke *stroke = static_cast<QuickStroke *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  if (!stroke || !region) {
    return OPERATOR_CANCELLED;
  }

  switch (event->type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      stroke->last_mval = int2(event->mval[0], event->mval[1]);
      quick_dabs_interpolated(*stroke, quick_region_to_uv(region, stroke->last_mval));
      image_paint_selection_targets_update(C, stroke->targets);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }

    case LEFTMOUSE: {
      if (event->val == KM_RELEASE) {
        quick_stroke_commit(C, stroke);
        quick_stroke_free(stroke);
        op->customdata = nullptr;
        ED_region_tag_redraw(region);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      if (event->val == KM_PRESS) {
        quick_stroke_cancel(C, stroke);
        quick_stroke_free(stroke);
        op->customdata = nullptr;
        ED_region_tag_redraw(region);
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    default:
      return OPERATOR_RUNNING_MODAL;
  }
}

static void image_select_quick_cancel(bContext *C, wmOperator *op)
{
  QuickStroke *stroke = static_cast<QuickStroke *>(op->customdata);
  if (!stroke) {
    return;
  }
  quick_stroke_cancel(C, stroke);
  quick_stroke_free(stroke);
  op->customdata = nullptr;
}

/** \} */

}  // namespace

void ED_image_paint_select_quick_cursor_update(wmWindowManager *wm, const bool enable)
{
  /* Removing by draw callback keeps this idempotent and never holds a pointer that a file load
   * could free. */
  WM_paint_cursor_remove_by_type(wm, reinterpret_cast<void *>(quick_cursor_draw), nullptr);
  if (enable) {
    WM_paint_cursor_activate(
        SPACE_IMAGE, RGN_TYPE_WINDOW, quick_cursor_poll, quick_cursor_draw, nullptr);
  }
}

void PAINT_OT_image_select_quick(wmOperatorType *ot)
{
  ot->name = "Quick Select";
  ot->idname = "PAINT_OT_image_select_quick";
  ot->description =
      "Paint a selection that spreads over similar pixels until it reaches an edge or a UV "
      "seam; Alt subtracts";

  ot->invoke = image_select_quick_invoke;
  ot->modal = image_select_quick_modal;
  ot->cancel = image_select_quick_cancel;
  ot->poll = image_select_quick_poll;
  /* The stroke opens and closes its own image undo step. */
  ot->flag = OPTYPE_REGISTER;
}

}  // namespace blender
