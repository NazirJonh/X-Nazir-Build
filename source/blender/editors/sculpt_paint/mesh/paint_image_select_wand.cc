/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Magic Wand for the Image Paint selection system ("Select by Color"), and "Refine Selection".
 *
 * A click builds the feature image of the source tile (the canvas image, or a chosen PBR
 * channel), samples the clicked pixel (Sample Size), and either floods a contiguous region or
 * marks every similar pixel, constrained by the UV-island map when UV Borders is on. The
 * resulting soft mask is post-processed (smooth, fill holes, grow/shrink), combined into every
 * selection target by the active select operation, and committed in one image undo step.
 *
 * The core is a minimax ("bottleneck") cost map: the flood cost of every pixel from the seed.
 * Selecting with any tolerance is then a linear threshold of that map, which is what makes the
 * drag-tolerance mode cheap: the press builds the map for the whole tolerance range, dragging
 * only re-thresholds it (the preview skips the post-processing), and the release runs the full
 * pipeline once.
 *
 * Refine Selection re-applies the wand's post-processing (smooth, fill holes, grow/shrink,
 * feather) to the current selection mask of every target, in one undo step. Works on any
 * selection — box, lasso, wand, quick select — so the refinement controls live in one place.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_report.hh"

#include "ED_image_paint_symmetry.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_image_select_gesture.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_region.hh"
#include "paint_image_select_source.hh"

namespace blender {

namespace image_select = ed::sculpt_paint::image_select;

namespace {

/** Width of the anti-aliased tolerance edge, as a fraction of the full tolerance range. */
constexpr float WAND_SOFTNESS = 0.03f;

/* -------------------------------------------------------------------- */
/** \name Wand parameters
 * \{ */

struct WandParams {
  float tolerance = 32.0f;        /* 0..255. */
  float normal_tolerance = 15.0f; /* Degrees 0..180. */
  /** Redo/Python override of the tolerance in the units of the source kind; negative = none. */
  float tolerance_override = -1.0f;
  int sample_size = 1;
  image_select::SourceComponent component = image_select::SourceComponent::RGB;
  image_select::SourceMetric metric = image_select::SourceMetric::Auto;
  bool contiguous = true;
  bool antialias = true;
  bool aa_edges = true;
  bool use_uv_bounds = true;
  bool same_island_only = false;
  bool connect_8 = false;
  bool normalize_range = false;
  bool symmetry_orig_sample = true;
  bool drag_tolerance = false;
  int uv_margin_px = 2;
  float smooth_px = 1.0f;
  float feather_px = 0.0f;
  float grow_px = 0.0f;
  int fill_holes_px = 0;
  eSelectOp sel_op = SEL_OP_SET;
};

/** Read the tool settings into the parameter block; the operator properties win when set. */
static WandParams wand_params_from_op(wmOperator *op, const Scene *scene)
{
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  WandParams params;
  params.tolerance = imapaint.select_tolerance;
  params.normal_tolerance = imapaint.select_normal_tolerance;
  params.sample_size = imapaint.select_sample_size;
  params.component = image_select::SourceComponent(imapaint.select_component);
  params.metric = image_select::SourceMetric(imapaint.select_metric);
  params.contiguous = (imapaint.select_flag & IMAGE_PAINT_SELECT_CONTIGUOUS) != 0;
  params.antialias = (imapaint.select_flag & IMAGE_PAINT_SELECT_ANTIALIAS) != 0;
  params.aa_edges = (imapaint.select_flag & IMAGE_PAINT_SELECT_AA_EDGES) != 0;
  params.use_uv_bounds = (imapaint.select_flag & IMAGE_PAINT_SELECT_USE_UV_BOUNDS) != 0;
  params.same_island_only = (imapaint.select_flag & IMAGE_PAINT_SELECT_SAME_ISLAND_ONLY) != 0;
  params.connect_8 = (imapaint.select_flag & IMAGE_PAINT_SELECT_CONNECT_8) != 0;
  params.normalize_range = (imapaint.select_flag & IMAGE_PAINT_SELECT_NORMALIZE_RANGE) != 0;
  params.symmetry_orig_sample = (imapaint.select_flag & IMAGE_PAINT_SELECT_SYMMETRY_ORIG_SAMPLE) !=
                                0;
  params.drag_tolerance = (imapaint.select_flag & IMAGE_PAINT_SELECT_DRAG_TOLERANCE) != 0;
  params.uv_margin_px = imapaint.select_uv_margin_px;
  params.smooth_px = imapaint.select_smooth_px;
  params.feather_px = imapaint.select_feather_px;
  params.grow_px = imapaint.select_grow_px;
  params.fill_holes_px = imapaint.select_fill_holes_px;

  if (op) {
    params.tolerance_override = RNA_float_get(op->ptr, "tolerance");
    params.sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  }
  return params;
}

/** Largest tolerance in the UI units of the source kind (degrees for normals). */
static float wand_tolerance_max(const bool is_normal)
{
  return is_normal ? 180.0f : 255.0f;
}

/** The tolerance in the UI units of the source kind. */
static float wand_tolerance_ui_get(const WandParams &params, const bool is_normal)
{
  if (params.tolerance_override >= 0.0f) {
    return params.tolerance_override;
  }
  return is_normal ? params.normal_tolerance : params.tolerance;
}

/** UI-unit tolerance to the 0..1 distance range of the feature metrics. */
static float wand_tolerance_unit(const float ui_value, const bool is_normal)
{
  return math::clamp(ui_value / wand_tolerance_max(is_normal), 0.0f, 1.0f);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cost fields
 *
 * Per seed (the click plus its canvas-symmetry copies) the distance/cost field of the source tile.
 * \{ */

struct WandSeed {
  int2 px = int2(0, 0);
  /** Minimax cost map (contiguous mode). */
  Array<uint16_t> cost;
  /** Distance to the reference sample (non-contiguous mode). */
  Array<float> dist;
};

struct WandField {
  image_select::SelectSource source;
  int2 size = int2(0, 0);
  bool contiguous = true;
  bool is_normal = false;
  image_select::UVIslandMap barrier;
  Vector<WandSeed> seeds;
  /** Kept for the anti-aliased edge pass. */
  image_select::FeatureImage feature;

  const image_select::UVIslandMap *barrier_get() const
  {
    return this->barrier.is_valid() ? &this->barrier : nullptr;
  }
};

/**
 * Build the fields of every seed. \a max_tolerance (0..1) bounds the flood of a contiguous
 * selection, so a plain click only visits the neighborhood of its tolerance; the drag mode
 * passes 1.
 *
 * \param r_outside_uv: set when UV Borders is on and the click is not inside a UV island.
 */
static bool wand_field_build(const bContext *C,
                             const WandParams &params,
                             const image_select::SelectSource &source,
                             const float2 &click_uv,
                             const float max_tolerance,
                             WandField &r_field,
                             bool &r_outside_uv)
{
  r_outside_uv = false;
  const Scene *scene = CTX_data_scene(C);

  r_field.feature = image_select::feature_image_build(source.image,
                                                      &source.iuser,
                                                      source.tile_number,
                                                      params.component,
                                                      params.metric,
                                                      source.is_normal,
                                                      params.normalize_range);
  const image_select::FeatureImage &feature = r_field.feature;
  if (feature.is_empty()) {
    return false;
  }
  r_field.source = source;
  r_field.size = feature.size;
  r_field.contiguous = params.contiguous;
  r_field.is_normal = source.is_normal;

  if (params.use_uv_bounds) {
    r_field.barrier = image_select::uv_island_map_build(
        C, source.image, source.tile_number, feature.size.x, feature.size.y, params.uv_margin_px);
  }
  const image_select::UVIslandMap *barrier = r_field.barrier_get();

  const float2 tile_origin = image_select_udim_tile_uv_origin(source.tile_number);
  const auto pixel_of = [&](const float2 &uv, int2 &r_px) {
    const float2 px_f = (uv - tile_origin) * float2(feature.size);
    r_px = int2(int(std::floor(px_f.x)), int(std::floor(px_f.y)));
    return r_px.x >= 0 && r_px.y >= 0 && r_px.x < feature.size.x && r_px.y < feature.size.y;
  };

  int2 click_px;
  if (!pixel_of(click_uv, click_px)) {
    return false;
  }
  if (barrier && barrier->island_at(click_px) < 0 && !barrier->overlap_at(click_px)) {
    r_outside_uv = true;
    return false;
  }
  Array<float> click_sample(feature.dims);
  image_select::feature_sample_window(feature, click_px, params.sample_size, click_sample);

  /* The click and, when the canvas symmetry affects selections, its copies. */
  Vector<float2> seed_uvs;
  seed_uvs.append(click_uv);
  const std::optional<ed::image_paint_symmetry::CanvasSymmetry> symmetry =
      (scene && scene->toolsettings) ?
          ed::image_paint_symmetry::from_settings(*scene->toolsettings,
                                                  IMAGE_PAINT_SYMMETRY_LINE_AFFECT_SELECTION) :
          std::nullopt;
  if (symmetry) {
    for (const int copy : IndexRange(symmetry->copies_num())) {
      seed_uvs.append(symmetry->apply(copy, click_uv));
    }
  }

  const int64_t pixel_num = int64_t(feature.size.x) * feature.size.y;
  const auto passable = [&](const int2 from, const int2 to) {
    return barrier->passable(from, to);
  };
  for (const int seed_i : seed_uvs.index_range()) {
    int2 px;
    if (!pixel_of(seed_uvs[seed_i], px)) {
      continue;
    }
    if (barrier && barrier->island_at(px) < 0 && !barrier->overlap_at(px)) {
      continue;
    }
    Array<float> own_sample;
    const float *sample = click_sample.data();
    if (seed_i > 0 && !params.symmetry_orig_sample) {
      own_sample.reinitialize(feature.dims);
      image_select::feature_sample_window(feature, px, params.sample_size, own_sample);
      sample = own_sample.data();
    }

    WandSeed seed;
    seed.px = px;
    if (params.contiguous) {
      /* The minimax flood evaluates the distance lazily, so only visited pixels pay for it
       * (no full-tile distance buffer in contiguous mode). */
      seed.cost.reinitialize(pixel_num);
      FunctionRef<bool(int2, int2)> passable_ref;
      if (barrier) {
        passable_ref = passable;
      }
      image_select::minimax_cost_map(
          [&](const int64_t i) {
            return image_select::feature_distance(
                feature, sample, &feature.data[i * feature.dims]);
          },
          feature.size,
          px,
          params.connect_8,
          max_tolerance,
          passable_ref,
          seed.cost);
    }
    else {
      Array<float> dist(pixel_num);
      threading::parallel_for(IndexRange(pixel_num), 4096, [&](const IndexRange range) {
        for (const int64_t i : range) {
          dist[i] = image_select::feature_distance(
              feature, sample, &feature.data[i * feature.dims]);
        }
      });
      seed.dist = std::move(dist);
    }
    r_field.seeds.append(std::move(seed));
  }
  return !r_field.seeds.is_empty();
}

/**
 * Threshold the fields at \a tolerance (0..1): the union of the seeds' selections as a soft
 * mask, before any post-processing.
 */
static Array<float> wand_field_threshold(const WandField &field,
                                         const WandParams &params,
                                         const float tolerance,
                                         const float softness)
{
  const int64_t pixel_num = int64_t(field.size.x) * field.size.y;
  const image_select::UVIslandMap *barrier = field.barrier_get();
  Array<float> mask(pixel_num, 0.0f);
  Array<float> part(pixel_num);

  for (const WandSeed &seed : field.seeds) {
    if (field.contiguous) {
      image_select::soft_threshold(seed.cost, field.size, seed.px, tolerance, softness, part);
    }
    else {
      image_select::threshold_global(seed.dist, tolerance, part);
      if (barrier) {
        const int32_t seed_island = barrier->island_at(seed.px);
        threading::parallel_for(IndexRange(pixel_num), 4096, [&](const IndexRange range) {
          for (const int64_t i : range) {
            if (part[i] <= 0.0f || barrier->overlap[i]) {
              continue;
            }
            const bool outside = barrier->island[i] < 0;
            if (outside || (params.same_island_only && barrier->island[i] != seed_island)) {
              part[i] = 0.0f;
            }
          }
        });
      }
    }
    threading::parallel_for(IndexRange(pixel_num), 4096, [&](const IndexRange range) {
      for (const int64_t i : range) {
        mask[i] = std::max(mask[i], part[i]);
      }
    });
  }
  return mask;
}

/** Minimum share of the selected color a blended pixel needs to join the selection. */
constexpr float WAND_AA_MIN_COVERAGE = 0.25f;

/**
 * Add the anti-aliased pixels along the selection edge. An edge pixel is a blend of the selected
 * color (its selected neighbor) and the color of an unselected neighbor: it lies on the segment
 * between the two in feature space. Such a pixel is too far from the sample for the tolerance, so
 * the flood stops in front of it. One ring only, so the pass cannot creep along a gradient.
 * Normals and hue are not linear features and are skipped.
 */
static void wand_include_aa_edges(MutableSpan<float> mask,
                                  const WandField &field,
                                  const float tolerance)
{
  const image_select::FeatureImage &feature = field.feature;
  const int dims = feature.dims;
  if (feature.is_normal || feature.circular || dims < 1 || dims > 4) {
    return;
  }
  const int2 size = field.size;
  const image_select::UVIslandMap *barrier = field.barrier_get();
  /* The residual tolerance keeps a floor: at tolerance 0 the blend is still an edge pixel. */
  const float residual_max = std::max(tolerance, 0.04f);
  const int2 offsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};

  Array<uint8_t> added(mask.size(), 0);
  threading::parallel_for(IndexRange(size.y), 8, [&](const IndexRange rows) {
    for (const int y : rows) {
      for (const int x : IndexRange(size.x)) {
        const int64_t idx = int64_t(y) * size.x + x;
        if (mask[idx] >= 0.5f) {
          continue;
        }
        const float *fp = feature.pixel(x, y);
        bool include = false;
        for (const int2 &so : offsets) {
          const int2 s(x + so.x, y + so.y);
          if (s.x < 0 || s.y < 0 || s.x >= size.x || s.y >= size.y ||
              mask[int64_t(s.y) * size.x + s.x] < 0.5f ||
              (barrier && !barrier->passable(s, int2(x, y))))
          {
            continue;
          }
          const float *fa = feature.pixel(s.x, s.y);
          for (const int2 &qo : offsets) {
            const int2 q(x + qo.x, y + qo.y);
            if (q == s || q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y ||
                mask[int64_t(q.y) * size.x + q.x] >= 0.5f)
            {
              continue;
            }
            const float *fb = feature.pixel(q.x, q.y);
            float d[4], proj[4];
            float dd = 0.0f, dot = 0.0f;
            for (const int c : IndexRange(dims)) {
              d[c] = fb[c] - fa[c];
              dd += d[c] * d[c];
              dot += (fp[c] - fa[c]) * d[c];
            }
            if (dd < 1e-6f) {
              continue;
            }
            const float t = math::clamp(dot / dd, 0.0f, 1.0f);
            for (const int c : IndexRange(dims)) {
              proj[c] = fa[c] + t * d[c];
            }
            if ((1.0f - t) >= WAND_AA_MIN_COVERAGE &&
                image_select::feature_distance(feature, fp, proj) <= residual_max)
            {
              include = true;
              break;
            }
          }
          if (include) {
            break;
          }
        }
        added[idx] = include;
      }
    }
  });
  for (const int64_t i : mask.index_range()) {
    if (added[i]) {
      mask[i] = 1.0f;
    }
  }
}

/* Shared by the wand (barrier-aware) and Refine Selection (barrier = nullptr: the runtime mask
 * has no island information of its own). */

/** Smooth, then fill holes of a selection mask. */
static void selection_mask_refine_smooth_and_fill(MutableSpan<float> mask,
                                                  const int2 size,
                                                  const float smooth_px,
                                                  const int fill_holes_px,
                                                  const image_select::UVIslandMap *barrier)
{
  const int64_t pixel_num = mask.size();
  if (smooth_px > 0.0f) {
    Array<float> blurred(pixel_num);
    image_select::gaussian_blur_barrier(mask, size, smooth_px, barrier, blurred);
    for (const int64_t i : IndexRange(pixel_num)) {
      mask[i] = (blurred[i] >= 0.5f) ? 1.0f : 0.0f;
    }
  }
  if (fill_holes_px > 0) {
    image_select::mask_fill_holes(mask, size, fill_holes_px);
  }
}

/** Grow (positive) or shrink (negative) a selection mask. */
static void selection_mask_refine_grow_shrink(MutableSpan<float> mask,
                                              const int2 size,
                                              const float grow_px)
{
  if (grow_px >= 1.0f) {
    image_select::mask_grow(mask, size, int(grow_px));
  }
  else if (grow_px <= -1.0f) {
    image_select::mask_shrink(mask, size, int(-grow_px));
  }
}

/** Smooth, fill holes and grow/shrink the thresholded mask, then keep it inside the islands. */
static void wand_mask_postprocess(MutableSpan<float> mask,
                                  const WandField &field,
                                  const WandParams &params,
                                  const float tolerance)
{
  const int2 size = field.size;
  const image_select::UVIslandMap *barrier = field.barrier_get();
  const int64_t pixel_num = mask.size();
  selection_mask_refine_smooth_and_fill(
      mask, size, params.smooth_px, params.fill_holes_px, barrier);
  if (params.aa_edges) {
    wand_include_aa_edges(mask, field, tolerance);
  }
  selection_mask_refine_grow_shrink(mask, size, params.grow_px);
  /* The smoothing and the morphology may have crossed a seam. */
  if (barrier) {
    for (const int64_t i : IndexRange(pixel_num)) {
      if (mask[i] > 0.0f && barrier->island[i] < 0 && !barrier->overlap[i]) {
        mask[i] = 0.0f;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Selection write
 * \{ */

/**
 * Combine the source-tile mask into every selection target by \a sel_op. Targets whose
 * resolution differs from the source get a bilinear resample of the same tile; other UDIM tiles
 * are untouched (the wand works per tile).
 */
static void wand_mask_write(const Span<ImagePaintSelectionTarget> targets,
                            const image_select::SelectSource &source,
                            const int2 src_size,
                            const Span<float> src_mask,
                            const eSelectOp sel_op)
{
  for (const ImagePaintSelectionTarget &target : targets) {
    /* The tile's pixel size comes from the image buffer itself (targets may differ from the
     * source resolution). */
    ImageUser iuser = target.iuser;
    iuser.tile = source.tile_number;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(target.image, &iuser, &lock);
    if (!ibuf) {
      continue;
    }
    const int2 dst_size(ibuf->x, ibuf->y);
    BKE_image_release_ibuf(target.image, ibuf, lock);

    /* The runtime's cached mask: write into it, never free it. */
    ImBuf *tile_mask = BKE_image_paint_selection_mask_get(
        target.image, source.tile_number, dst_size.x, dst_size.y);
    if (!tile_mask || !tile_mask->float_data()) {
      continue;
    }
    const MutableSpan<float> dst(tile_mask->float_data_for_write(),
                                 int64_t(dst_size.x) * dst_size.y);
    if (dst_size == src_size) {
      image_select::mask_combine(dst, src_mask, sel_op);
    }
    else {
      Array<float> resampled(dst.size());
      image_select::mask_resample(src_mask, src_size, resampled, dst_size);
      image_select::mask_combine(dst, resampled, sel_op);
    }
  }
}

/** UV bounds of the selected pixels of \a mask; false when nothing is selected. */
static bool wand_mask_bounds_get(const Span<float> mask,
                                 const int2 size,
                                 const float2 &tile_origin,
                                 rctf &r_bounds)
{
  int2 min_px(size.x, size.y);
  int2 max_px(-1, -1);
  for (const int y : IndexRange(size.y)) {
    const Span<float> row = mask.slice(int64_t(y) * size.x, size.x);
    for (const int x : IndexRange(size.x)) {
      if (row[x] > 0.5f) {
        min_px = math::min(min_px, int2(x, y));
        max_px = math::max(max_px, int2(x, y));
      }
    }
  }
  if (max_px.x < 0) {
    return false;
  }
  BLI_rctf_init_minmax(&r_bounds);
  BLI_rctf_do_minmax_v(&r_bounds, tile_origin + float2(min_px) / float2(size));
  BLI_rctf_do_minmax_v(&r_bounds, tile_origin + float2(max_px + int2(1)) / float2(size));
  return true;
}

/**
 * Apply the final \a mask to every target: SET clears the old selection, Face/Island expansion
 * runs in the same order as the gesture tools (Subtract before the pixels change, the others
 * after), and the edge policy is always set explicitly so a feather of an earlier tool never
 * leaks in.
 */
static void wand_selection_apply(bContext *C,
                                 const WandParams &params,
                                 const image_select::SelectSource &source,
                                 const Span<ImagePaintSelectionTarget> targets,
                                 const int2 size,
                                 const Span<float> mask)
{
  rctf bounds;
  const bool have_bounds = wand_mask_bounds_get(
      mask, size, image_select_udim_tile_uv_origin(source.tile_number), bounds);
  const eSelectOp sel_op = params.sel_op;

  if (sel_op == SEL_OP_SUB && have_bounds) {
    for (const ImagePaintSelectionTarget &target : targets) {
      image_paint_selection_expand(C, target.image, sel_op, &bounds);
    }
  }
  if (sel_op == SEL_OP_SET) {
    for (const ImagePaintSelectionTarget &target : targets) {
      BKE_image_paint_selection_mask_free(target.image);
    }
  }

  wand_mask_write(targets, source, size, mask, sel_op);

  if (sel_op != SEL_OP_SUB && have_bounds) {
    for (const ImagePaintSelectionTarget &target : targets) {
      image_paint_selection_expand(C, target.image, sel_op, &bounds);
    }
  }

  image_paint_selection_edge_policy_apply(targets, params.feather_px);
  image_paint_selection_targets_update(C, targets);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static bool image_select_wand_poll(bContext *C)
{
  return image_paint_selection_image_poll(C);
}

static wmOperatorStatus image_select_wand_exec(bContext *C, wmOperator *op)
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  const Scene *scene = CTX_data_scene(C);
  if (!sima || !scene || !scene->toolsettings) {
    return OPERATOR_CANCELLED;
  }

  const float2 click_uv(RNA_float_get(op->ptr, "location_u"),
                        RNA_float_get(op->ptr, "location_v"));
  const WandParams params = wand_params_from_op(op, scene);

  image_select::SelectSource source;
  if (!image_select::select_source_resolve_or_report(C, op->reports, click_uv, source)) {
    return OPERATOR_CANCELLED;
  }
  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_targets_get(C, sima);
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  const float tolerance = wand_tolerance_unit(wand_tolerance_ui_get(params, source.is_normal),
                                              source.is_normal);
  const float softness = params.antialias ? WAND_SOFTNESS : 0.0f;

  WandField field;
  bool outside_uv = false;
  if (!wand_field_build(
          C, params, source, click_uv, math::min(1.0f, tolerance + softness), field, outside_uv))
  {
    if (outside_uv) {
      BKE_report(op->reports, RPT_WARNING, "The click is outside the UV islands");
    }
    return OPERATOR_CANCELLED;
  }
  Array<float> mask = wand_field_threshold(field, params, tolerance, softness);
  wand_mask_postprocess(mask, field, params, tolerance);

  image_paint_selection_undo_begin("Magic Wand", targets);
  wand_selection_apply(C, params, source, targets, field.size, mask);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drag-tolerance mode
 *
 * With #IMAGE_PAINT_SELECT_DRAG_TOLERANCE the press builds the cost fields for the whole
 * tolerance range once, and dragging horizontally moves the threshold through them. The preview
 * writes the plain thresholded mask (no smoothing / morphology) into every target; the release
 * restores the snapshots and runs the full pipeline once, in one undo step.
 * \{ */

struct WandDrag {
  WandParams params;
  WandField field;
  Vector<ImagePaintSelectionTarget> targets;
  Vector<ImagePaintSelectionMaskSnapshot> snapshots;
  /** Snapshots of the existing mask tiles other than the source tile (SET preview clears them). */
  Vector<ImagePaintSelectionMaskSnapshot> other_snapshots;
  int press_x = 0;
  float tolerance_start = 0.0f;
  float tolerance = 0.0f;
  ScrArea *area = nullptr;
};

static void wand_drag_free(WandDrag *drag)
{
  image_paint_selection_mask_snapshots_free(drag->snapshots);
  image_paint_selection_mask_snapshots_free(drag->other_snapshots);
  MEM_delete(drag);
}

/**
 * Snapshot every existing mask tile of every target except \a skip_tile (the source tile, covered
 * by the main snapshot). Only tiles with a live mask are captured.
 */
static Vector<ImagePaintSelectionMaskSnapshot> wand_drag_other_tiles_snapshots(
    const Span<ImagePaintSelectionTarget> targets, const int skip_tile)
{
  Vector<ImagePaintSelectionMaskSnapshot> snapshots;
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *itile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      if (itile->tile_number == skip_tile) {
        continue;
      }
      const ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(target.image,
                                                                     itile->tile_number);
      if (!tile_mask || !tile_mask->float_data()) {
        continue;
      }
      ImagePaintSelectionMaskSnapshot snapshot;
      snapshot.image = target.image;
      snapshot.tile_number = itile->tile_number;
      snapshot.orig = IMB_dupImBuf(tile_mask);
      snapshots.append(snapshot);
    }
  }
  return snapshots;
}

static void wand_drag_status_set(WandDrag &drag)
{
  if (!drag.area) {
    return;
  }
  char text[64];
  if (drag.field.is_normal) {
    SNPRINTF(text, "Magic Wand tolerance: %.0f\xC2\xB0", drag.tolerance);
  }
  else {
    SNPRINTF(text, "Magic Wand tolerance: %.0f", drag.tolerance);
  }
  ED_area_status_text(drag.area, text);
}

/** Rewrite the targets from their snapshots with the mask of the current tolerance. */
static void wand_drag_preview(bContext *C, WandDrag &drag)
{
  const float softness = drag.params.antialias ? WAND_SOFTNESS : 0.0f;
  const Array<float> mask = wand_field_threshold(
      drag.field,
      drag.params,
      wand_tolerance_unit(drag.tolerance, drag.field.is_normal),
      softness);
  image_paint_selection_mask_snapshots_restore(drag.snapshots);
  if (drag.params.sel_op == SEL_OP_SET) {
    /* A SET commit clears every other tile, so the preview shows the same. */
    for (const ImagePaintSelectionMaskSnapshot &snapshot : drag.other_snapshots) {
      ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(snapshot.image,
                                                               snapshot.tile_number);
      if (tile_mask && tile_mask->float_data()) {
        std::memset(tile_mask->float_data_for_write(),
                    0,
                    sizeof(float) * size_t(tile_mask->x) * size_t(tile_mask->y));
      }
    }
  }
  wand_mask_write(drag.targets, drag.field.source, drag.field.size, mask, drag.params.sel_op);
  image_paint_selection_targets_update(C, drag.targets);
  wand_drag_status_set(drag);
}

static void wand_drag_close_ui(bContext *C, WandDrag *drag)
{
  if (drag->area) {
    ED_area_status_text(drag->area, nullptr);
  }
  WM_cursor_modal_restore(CTX_wm_window(C));
}

static void wand_drag_restore_and_close(bContext *C, WandDrag *drag)
{
  image_paint_selection_mask_snapshots_restore_for_cancel(drag->snapshots);
  image_paint_selection_mask_snapshots_restore(drag->other_snapshots);
  wand_drag_close_ui(C, drag);
}

static wmOperatorStatus image_select_wand_drag_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event,
                                                      const WandParams &params,
                                                      const float2 &click_uv)
{
  const SpaceImage *sima = CTX_wm_space_image(C);

  auto *drag = MEM_new<WandDrag>(__func__);
  drag->params = params;
  image_select::SelectSource source;
  bool outside_uv = false;
  if (!image_select::select_source_resolve_or_report(C, op->reports, click_uv, source) ||
      !wand_field_build(C, params, source, click_uv, 1.0f, drag->field, outside_uv))
  {
    if (outside_uv) {
      BKE_report(op->reports, RPT_WARNING, "The click is outside the UV islands");
    }
    MEM_delete(drag);
    return OPERATOR_CANCELLED;
  }
  drag->targets = image_paint_selection_targets_get(C, sima);
  if (drag->targets.is_empty()) {
    MEM_delete(drag);
    return OPERATOR_CANCELLED;
  }

  image_paint_selection_undo_begin("Magic Wand", drag->targets);
  drag->snapshots = image_paint_selection_mask_snapshots_create(drag->targets, source.tile_number);
  if (drag->params.sel_op == SEL_OP_SET) {
    drag->other_snapshots = wand_drag_other_tiles_snapshots(drag->targets, source.tile_number);
  }

  drag->press_x = event->mval[0];
  drag->tolerance_start = wand_tolerance_ui_get(params, source.is_normal);
  drag->tolerance = drag->tolerance_start;
  drag->area = CTX_wm_area(C);
  wand_drag_preview(C, *drag);

  op->customdata = drag;
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EW_SCROLL);
  return OPERATOR_RUNNING_MODAL;
}

/** Final selection at the dragged tolerance: full post-processing on all targets. */
static void wand_drag_commit(bContext *C, WandDrag &drag)
{
  const Scene *scene = CTX_data_scene(C);
  const image_select::SelectSource &source = drag.field.source;
  const float softness = drag.params.antialias ? WAND_SOFTNESS : 0.0f;

  image_paint_selection_mask_snapshots_restore(drag.snapshots);
  image_paint_selection_mask_snapshots_restore(drag.other_snapshots);
  Array<float> mask = wand_field_threshold(
      drag.field,
      drag.params,
      wand_tolerance_unit(drag.tolerance, drag.field.is_normal),
      softness);
  wand_mask_postprocess(
      mask, drag.field, drag.params, wand_tolerance_unit(drag.tolerance, drag.field.is_normal));
  wand_selection_apply(C, drag.params, source, drag.targets, drag.field.size, mask);

  /* Remember the dragged tolerance as the tool setting. */
  ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  if (drag.field.is_normal) {
    imapaint.select_normal_tolerance = drag.tolerance;
  }
  else {
    imapaint.select_tolerance = drag.tolerance;
  }
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
}

static wmOperatorStatus image_select_wand_drag_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  WandDrag *drag = static_cast<WandDrag *>(op->customdata);
  if (!drag) {
    return OPERATOR_CANCELLED;
  }
  switch (event->type) {
    case MOUSEMOVE:
    case INBETWEEN_MOUSEMOVE: {
      const float max_value = wand_tolerance_max(drag->field.is_normal);
      /* One pixel of travel is one unit of the 0..255 scale (proportionally less for degrees). */
      const float step = max_value / 255.0f;
      drag->tolerance = math::clamp(
          drag->tolerance_start + float(event->mval[0] - drag->press_x) * step, 0.0f, max_value);
      wand_drag_preview(C, *drag);
      return OPERATOR_RUNNING_MODAL;
    }
    case LEFTMOUSE: {
      if (event->val == KM_RELEASE) {
        wand_drag_commit(C, *drag);
        wand_drag_close_ui(C, drag);
        ED_image_undo_push_end();
        wand_drag_free(drag);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    case EVT_ESCKEY:
    case RIGHTMOUSE: {
      if (event->val == KM_PRESS) {
        wand_drag_restore_and_close(C, drag);
        image_paint_selection_targets_update(C, drag->targets);
        ED_image_undo_push_end();
        wand_drag_free(drag);
        op->customdata = nullptr;
        return OPERATOR_CANCELLED;
      }
      return OPERATOR_RUNNING_MODAL;
    }
    default:
      return OPERATOR_RUNNING_MODAL;
  }
}

static void image_select_wand_cancel(bContext *C, wmOperator *op)
{
  WandDrag *drag = static_cast<WandDrag *>(op->customdata);
  if (!drag) {
    return;
  }
  wand_drag_restore_and_close(C, drag);
  image_paint_selection_targets_update(C, drag->targets);
  ED_image_undo_push_end();
  wand_drag_free(drag);
  op->customdata = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator callbacks
 * \{ */

static wmOperatorStatus image_select_wand_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  const ARegion *region = CTX_wm_region(C);
  const Scene *scene = CTX_data_scene(C);
  if (!sima || !region || !scene || !scene->toolsettings) {
    return OPERATOR_CANCELLED;
  }
  /* Any other floating session must be settled before this tool edits the selection. */
  image_select_floating_sessions_end_all(C, sima);

  float uv[2];
  ui::view2d_region_to_view(
      &region->v2d, float(event->mval[0]), float(event->mval[1]), &uv[0], &uv[1]);
  RNA_float_set(op->ptr, "location_u", uv[0]);
  RNA_float_set(op->ptr, "location_v", uv[1]);

  const WandParams params = wand_params_from_op(op, scene);
  if (params.drag_tolerance) {
    return image_select_wand_drag_invoke(C, op, event, params, float2(uv[0], uv[1]));
  }
  return image_select_wand_exec(C, op);
}

/** \} */

}  // namespace

void PAINT_OT_image_select_wand(wmOperatorType *ot)
{
  ot->name = "Magic Wand";
  ot->idname = "PAINT_OT_image_select_wand";
  ot->description =
      "Select similar pixels: click to select a contiguous area of similar color, or "
      "select every similar pixel in the tile at once";

  ot->exec = image_select_wand_exec;
  ot->invoke = image_select_wand_invoke;
  ot->modal = image_select_wand_drag_modal;
  ot->cancel = image_select_wand_cancel;
  ot->poll = image_select_wand_poll;
  /* The operator opens and closes its own image undo step. */
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_select_operation_simple(ot);
  PropertyRNA *prop = RNA_def_float(
      ot->srna, "tolerance", -1.0f, -1.0f, 255.0f, "Tolerance", "", -1.0f, 255.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_float(ot->srna, "location_u", 0.0f, -FLT_MAX, FLT_MAX, "U", "", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_float(ot->srna, "location_v", 0.0f, -FLT_MAX, FLT_MAX, "V", "", 0.0f, 1.0f);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/* -------------------------------------------------------------------- */
/** \name Refine Selection
 * \{ */

namespace {

static bool image_select_refine_poll(bContext *C)
{
  return image_paint_selection_image_poll(C);
}

static wmOperatorStatus image_select_refine_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  Scene *scene = CTX_data_scene(C);
  if (!sima || !scene || !scene->toolsettings) {
    return OPERATOR_CANCELLED;
  }
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;

  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_targets_get(C, sima);
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  image_paint_selection_undo_begin("Refine Selection", targets);

  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *itile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ImBuf *tile_mask = BKE_image_paint_selection_mask_lookup(target.image, itile->tile_number);
      if (!tile_mask || !tile_mask->float_data()) {
        continue; /* Tiles without a selection mask are untouched. */
      }
      const int2 size(tile_mask->x, tile_mask->y);
      const MutableSpan<float> mask(tile_mask->float_data_for_write(), int64_t(size.x) * size.y);
      selection_mask_refine_smooth_and_fill(
          mask, size, imapaint.select_smooth_px, imapaint.select_fill_holes_px, nullptr);
      selection_mask_refine_grow_shrink(mask, size, imapaint.select_grow_px);
    }
  }
  /* Always explicit, so a feather of an earlier tool never leaks in. */
  image_paint_selection_edge_policy_apply(targets, imapaint.select_feather_px);

  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

}  // namespace

void PAINT_OT_image_select_refine(wmOperatorType *ot)
{
  ot->name = "Refine Selection";
  ot->idname = "PAINT_OT_image_select_refine";
  ot->description =
      "Re-apply the selection refinement controls (smooth, fill holes, grow/shrink, feather) "
      "to the current selection";
  ot->exec = image_select_refine_exec;
  ot->poll = image_select_refine_poll;
  /* The operator opens and closes its own image undo step. */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender
