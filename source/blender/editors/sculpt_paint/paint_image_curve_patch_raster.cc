/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_image_curve_patch_raster.hh"

#include <cfloat>
#include <optional>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_rect.h"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curve_patch.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_texture_types.h"

#include "ED_paint.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "mesh/paint_material_blend.hh"
#include "mesh/paint_material_source.hh"
#include "paint_curve_patch_effect_common.hh"
#include "paint_curve_patch_sampler.hh"
#include "paint_curve_patch_session.hh"
#include "paint_image_curve_patch.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Pure helpers
 * \{ */

float2 image_curve_patch_uv_to_ref_px(const float2 &uv,
                                      const float2 &ref_tile_uv_origin,
                                      const int2 &ref_tile_resolution)
{
  return (uv - ref_tile_uv_origin) * float2(ref_tile_resolution);
}

float2 image_curve_patch_ref_px_to_uv(const float2 &ref_px,
                                      const float2 &ref_tile_uv_origin,
                                      const int2 &ref_tile_resolution)
{
  return ref_px / float2(ref_tile_resolution) + ref_tile_uv_origin;
}

float4 image_curve_patch_blend_src_float(const float3 &brush_color_linear, const float alpha)
{
  /* Float image buffers are premultiplied in Blender's convention. */
  return float4(brush_color_linear.x * alpha,
                brush_color_linear.y * alpha,
                brush_color_linear.z * alpha,
                alpha);
}

void image_curve_patch_blend_src_byte(const float3 &brush_color_linear,
                                      const float alpha,
                                      const ColorSpace *byte_colorspace,
                                      uchar r_src[4])
{
  float3 color_cs = brush_color_linear;
  if (byte_colorspace != nullptr) {
    IMB_colormanagement_scene_linear_to_colorspace_v3(color_cs, byte_colorspace);
  }
  const float col_f[4] = {color_cs.x, color_cs.y, color_cs.z, alpha};
  rgba_float_to_uchar(r_src, col_f);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Geometry Rebuild
 * \{ */

namespace {

/* Resolve which UDIM tile the patch is anchored in and that tile's resolution, from the
 * session's first control point (spec §4.2, §9). */
struct ReferenceTile {
  int number = 0;
  float2 uv_origin = float2(0.0f, 0.0f);
  int2 resolution = int2(0, 0);
  bool valid = false;
};

static ReferenceTile resolve_reference_tile(const Image &image, const float2 &first_point_uv)
{
  ReferenceTile out;
  float local_uv[2];
  float ofs[2];
  const float uv_in[2] = {first_point_uv.x, first_point_uv.y};
  out.number = BKE_image_get_tile_from_pos(const_cast<Image *>(&image), uv_in, local_uv, ofs);
  out.uv_origin = float2(ofs[0], ofs[1]);

  ImageUser iuser = {};
  iuser.tile = out.number;
  iuser.framenr = image.lastframe;
  ImBuf *ibuf = BKE_image_acquire_ibuf(const_cast<Image *>(&image), &iuser, nullptr);
  if (ibuf == nullptr) {
    return out;
  }
  out.resolution = int2(ibuf->x, ibuf->y);
  out.valid = out.resolution.x > 0 && out.resolution.y > 0;
  BKE_image_release_ibuf(const_cast<Image *>(&image), ibuf, nullptr);
  return out;
}

}  // namespace

bke::CurvePatchParams image_curve_patch_params_resolve(ImageCurvePatchSession &session,
                                                       const Paint &paint,
                                                       const Brush &brush)
{
  /* `apply_brush_swap_axis = true`: 2D has no session-owned S hotkey (yet) that a live brush read
   * could clobber, unlike 3D. */
  const int brush_size = BKE_brush_size_get(&paint, &brush);
  session.frozen_patch_params.radius = session.radius_per_size * float(brush_size);
  session.frozen_patch_params.plane_normal = float3(0.0f, 0.0f, 1.0f);
  bke::CurvePatchParams params = curve_patch_params_live_overlay(
      brush, session.frozen_patch_params, brush_size, /*apply_brush_swap_axis=*/true);
  params.plane_normal = float3(0.0f, 0.0f, 1.0f);
  return params;
}

void image_curve_patch_geometry_rebuild(bContext *C, ImageCurvePatchSession &session)
{
  if (session.curve.points_num() == 0 || session.curve.curves_num() == 0) {
    session.doc.active_item().geometry.clear();
    return;
  }

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = paint ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush == nullptr) {
    session.doc.active_item().geometry.clear();
    return;
  }

  /* 1. Resolve the reference tile from the curve's first control point. */
  const Span<float3> uv_positions = session.curve.positions();
  const float2 first_uv(uv_positions[0].x, uv_positions[0].y);
  const ReferenceTile ref = resolve_reference_tile(*session.image, first_uv);
  if (!ref.valid) {
    session.doc.active_item().geometry.clear();
    return;
  }
  session.ref_tile_number = ref.number;
  session.ref_tile_uv_origin = ref.uv_origin;
  session.ref_tile_resolution = ref.resolution;

  /* 2. Project the canonical UV curve into reference-tile pixels (spec §6.1). */
  bke::CurvesGeometry &control_curve = session.doc.active_item().control_curve;
  control_curve = session.curve;
  auto project = [&](MutableSpan<float3> coords) {
    for (float3 &co : coords) {
      const float2 px = image_curve_patch_uv_to_ref_px(
          float2(co.x, co.y), session.ref_tile_uv_origin, session.ref_tile_resolution);
      co = float3(px.x, px.y, 0.0f);
    }
  };
  project(control_curve.positions_for_write());
  if (control_curve.handle_positions_left().has_value()) {
    project(control_curve.handle_positions_left_for_write());
  }
  if (control_curve.handle_positions_right().has_value()) {
    project(control_curve.handle_positions_right_for_write());
  }
  control_curve.tag_positions_changed();

  /* 3. Live-overlay parameters (spec §6.2). */
  session.doc.active_item().params = image_curve_patch_params_resolve(session, *paint, *brush);

  /* 4. Texture bindings (spec §8.3, §8.4). */
  curve_patch_texture_binding_from_brush(
      *brush, session.doc.active_item().params.radius, session.doc.texture);

  /* 5. Build from the (already-in-pixel-space) control curve. #PlanarSingleWindow is what pins
   * this to the single-window ribbon instead of windowed frames: a flat canvas has no surface to
   * wrap onto (spec §6.3; regression-tested by BKE `build_mode_selects_normals_and_strip_path`).
   * The tessellation itself lives in the core wrapper so that it stays one implementation shared
   * with the 3D path. */
  bke::curve_patch_build_from_control_curve(control_curve,
                                            session.doc.active_item().params,
                                            session.doc.texture.stamp_texture_weights_cdf,
                                            bke::CurvePatchBuildMode::PlanarSingleWindow,
                                            session.doc.active_item().geometry);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Pixel Writer
 * \{ */

namespace {

/* Reference-tile-pixel bounding box the patch can possibly touch (spec §7.1). Mirrors
 * `curve_patch_cull_tube_radius()`'s reach exactly -- if the two ever disagree, pixels the
 * sampler would have accepted get silently dropped before the sampler is even asked. */
struct CoverageBounds {
  float2 min = float2(0.0f);
  float2 max = float2(0.0f);
  bool valid = false;
};

static CoverageBounds compute_coverage_bounds(const bke::CurvePatchGeometry &geometry)
{
  CoverageBounds out;
  if (geometry.spline.is_empty()) {
    return out;
  }
  const float reach = curve_patch_max_radius(geometry) * float(M_SQRT2) +
                      geometry.ribbon_end_margin;

  float2 lo(FLT_MAX, FLT_MAX);
  float2 hi(-FLT_MAX, -FLT_MAX);
  for (const float3 &p : geometry.spline.poly_3d) {
    lo = math::min(lo, float2(p.x, p.y));
    hi = math::max(hi, float2(p.x, p.y));
  }
  out.min = lo - float2(reach, reach);
  out.max = hi + float2(reach, reach);
  out.valid = true;
  return out;
}

/* One 64x64 image-undo tile's worth of pixel indices this call should touch, already clamped to
 * the ImBuf's real bounds and to `coverage` (spec §7.2, §15.2). */
struct TileRegion {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0; /* [x0, x1) x [y0, y1) in ibuf pixel coordinates. */
};

/* Snapshot every 64x64 undo tile `region` overlaps into the session's OWN tile map, before a
 * single pixel of `region` is touched. `ED_image_paint_tile_push(..., find_prev=true)` only
 * captures a tile the FIRST time it sees it -- calling it AFTER blending (as
 * `ED_imapaint_dirty_region()` alone would) captures the just-painted pixels as if they were the
 * pristine "before" state, so every later restore-and-restamp restores to the previous patch
 * instead of the true pre-anchor canvas and the old patch never disappears. */
static void push_undo_tiles_for_region(const TileRegion &region,
                                       PaintTileMap &undo_tiles,
                                       Image &image,
                                       ImBuf &ibuf,
                                       ImageUser &iuser)
{
  const int tx0 = region.x0 >> ED_IMAGE_UNDO_TILE_BITS;
  const int ty0 = region.y0 >> ED_IMAGE_UNDO_TILE_BITS;
  const int tx1 = (region.x1 - 1) >> ED_IMAGE_UNDO_TILE_BITS;
  const int ty1 = (region.y1 - 1) >> ED_IMAGE_UNDO_TILE_BITS;
  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      ED_image_paint_tile_push(
          &undo_tiles, &image, &ibuf, &iuser, tx, ty, nullptr, nullptr, false, /*find_prev=*/true);
    }
  }
}

/**
 * One canvas this patch writes: either the Image Editor's own image (Mode=`Image`) or one
 * Principled channel map (Mode=`Material`).
 *
 * The 3D canvas resolves the same list through #init_image_paint_targets; this path cannot reuse
 * that struct because it carries an owned #ImageData with acquired PBVH buffers, which a flat
 * canvas has no use for. What matters is that both derive the LIST from the same
 * #BKE_paint_material_image_targets_get, so the two engines cannot disagree on which channels a
 * brush writes.
 */
struct RasterTarget {
  Image *image = nullptr;
  bool is_material_channel = false;
  bool is_color_channel = false;
  bool is_normal_channel = false;
  eMaterialPaintChannel channel = PAINT_MATERIAL_CHANNEL_BASE_COLOR;
  /** What this canvas paints where no source supplies a color: the channel RGB, the packed
   * direction for Normal, grey `(v, v, v)` for a scalar channel, or the brush color for the
   * plain image canvas. Scene-linear except for Normal, which is packed tangent data. */
  float3 flat_color = float3(0.0f);
  IMB_BlendMode blend = IMB_BLEND_MIX;
};

/**
 * Resolve every canvas this restamp writes.
 *
 * Rebuilt on every draw rather than frozen on the session: a live patch outlives panel edits, and
 * toggling a PBR channel changes WHICH images are written. The 3D path learned the same lesson
 * the hard way (see `ImageColorEffect::sync_targets`); here it costs nothing, because a flat
 * canvas holds no per-target state between restamps -- the undo tile map is keyed by #Image, so
 * a target that comes and goes is captured and restored correctly either way.
 */
static Vector<RasterTarget> resolve_targets(bContext *C,
                                            const ImageCurvePatchSession &session,
                                            const Paint &paint,
                                            const Brush &brush,
                                            const bool invert)
{
  Vector<RasterTarget> targets;
  Scene *scene = CTX_data_scene(C);
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  Object *ob = CTX_data_active_object(C);

  if (paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL && ob != nullptr &&
      brush.material_paint != nullptr)
  {
    const BrushMaterialPaint &brush_paint = *brush.material_paint;
    const Vector<PaintMaterialImageTarget> material_targets = BKE_paint_material_image_targets_get(
        *ob, paint_mode, &brush_paint, paint.visible_material_channels);
    for (const PaintMaterialImageTarget &material_target : material_targets) {
      if (material_target.image == nullptr) {
        continue;
      }
      RasterTarget target;
      target.image = material_target.image;
      target.is_material_channel = true;
      target.is_color_channel = material_target.is_color_channel;
      target.is_normal_channel = material_target.is_normal_channel;
      target.channel = material_target.channel;
      target.blend = IMB_BlendMode(
          BKE_paint_material_channel_blend_mode(brush_paint, material_target.channel, invert));
      /* The same three cases #init_image_paint_targets and `paint_2d_new_stroke()` resolve, in
       * the same order, so a channel means the same thing on all three canvases. */
      if (material_target.is_color_channel) {
        target.flat_color = BKE_paint_material_channel_color_get(
            brush_paint, paint, brush, material_target.channel, invert);
      }
      else if (material_target.is_normal_channel) {
        float tangent[3] = {0.0f, 0.0f, 1.0f};
        if (!invert) {
          copy_v3_v3(tangent, material_target.color);
        }
        float packed[3];
        BKE_pbr_normal_pack(tangent, false, packed);
        target.flat_color = float3(packed[0], packed[1], packed[2]);
      }
      else {
        const float value = invert ? BKE_paint_material_channel_default_value(
                                         material_target.channel) :
                                     material_target.value;
        target.flat_color = float3(value);
      }
      targets.append(target);
    }
    return targets;
  }

  /* Mode=`Image`: the editor's own canvas, painted with the brush color and the brush's own
   * blend mode -- the behavior this path has always had. (The 3D canvas deliberately pins its
   * plain-image case to Mix instead; the two differ because they always have, and quietly
   * changing either is not part of adding channel support.) */
  if (session.image != nullptr) {
    RasterTarget target;
    target.image = session.image;
    target.flat_color = float3(
        session.params.color[0], session.params.color[1], session.params.color[2]);
    target.blend = IMB_BlendMode(session.params.blend);
    targets.append(target);
  }
  return targets;
}

static void blend_tile_region(const TileRegion &region,
                              ImBuf &ibuf,
                              const CurvePatchItem &patch,
                              const CurvePatchTextureBinding &texture,
                              const CurvePatchStrokeContext &ctx,
                              const Brush &brush,
                              const RasterTarget &target,
                              const material::ChannelUvSampler *channel_sources,
                              const bool alpha_masking,
                              ImagePool &tex_pool,
                              const float2 &tile_uv_origin,
                              const int2 &tile_resolution,
                              const float2 &ref_tile_uv_origin,
                              const int2 &ref_tile_resolution)
{
  /* Whether this canvas takes its RGB from the RIBBON's own texture. Normal packs a direction and
   * a scalar channel carries one value, so neither can. Mirrors the 3D `target_paints_color`. */
  const bool target_paints_color = !target.is_normal_channel &&
                                   (target.is_color_channel || !target.is_material_channel);
  const bool target_has_source = target.is_material_channel && channel_sources != nullptr &&
                                 channel_sources->has_usable_source(target.channel);
  const bool target_masked_by_alpha = target.is_material_channel &&
                                      material::channel_uses_alpha_mask(alpha_masking,
                                                                        target.channel);
  const bool is_float = ibuf.float_data() != nullptr;
  float *float_data = is_float ? ibuf.float_data_for_write() : nullptr;
  uchar *byte_data = is_float ? nullptr : ibuf.byte_data_for_write();
  const ColorSpace *byte_colorspace = is_float ? nullptr : ibuf.byte_buffer.colorspace;

  const int row_count = region.y1 - region.y0;
  const int width = region.x1 - region.x0;
  threading::parallel_for(IndexRange(row_count), 1, [&](const IndexRange row_range) {
    /* The sampler is per-iteration, but the thread id it receives also selects the per-thread
     * node-tree exec stack in #RE_texture_evaluate and the per-thread RNG. Those live in the
     * shared #Tex, not in the sampler, so a constant id would let every worker mutate slot 0
     * at once. */
    const int thread_id = BLI_task_parallel_thread_id(nullptr);
    for (const int row_offset : row_range) {
      const int y_px = region.y0 + row_offset;

      Array<float3> positions(width);
      Array<float3> normals(width, float3(0.0f, 0.0f, 1.0f));
      for (const int i : IndexRange(width)) {
        const int x_px = region.x0 + i;
        /* This tile's own pixel -> UV -> reference-tile pixel space, since the patch geometry
         * was built in the REFERENCE tile's pixel space (spec §4.2, §9) -- for any tile other
         * than the reference one, or one with a different resolution, skipping this two-step
         * conversion (or round-tripping through the same tile's own origin/resolution twice,
         * which cancels out) would sample the geometry at the wrong location. */
        const float2 uv = image_curve_patch_ref_px_to_uv(
            float2(float(x_px) + 0.5f, float(y_px) + 0.5f), tile_uv_origin, tile_resolution);
        const float2 ref_px = image_curve_patch_uv_to_ref_px(
            uv, ref_tile_uv_origin, ref_tile_resolution);
        positions[i] = float3(ref_px.x, ref_px.y, 0.0f);
      }

      CurvePatchSourceGeometry source;
      source.positions = positions;
      source.normals = normals;
      source.orig_positions = nullptr;
      source.indices_are_mesh_verts = false;

      CurvePatchSampler sampler(patch, texture, ctx, brush, source, /*mask=*/{}, tex_pool);

      for (const int i : IndexRange(width)) {
        const std::optional<CurvePatchSample> sample = sampler.sample(i, thread_id);
        if (!sample) {
          continue;
        }
        /* The Alpha channel masks every other channel's write, exactly as it does for a dab and
         * for the 3D canvas. Sampled in the ribbon's own frame so the mask turns with the curve
         * together with the color it is masking. A sample with no patch frame has nowhere to read
         * it from -- the flat canvas has no brush-mapping fallback, having no view state in the
         * writer at all -- so it goes unmasked rather than guessing. */
        float alpha_factor = 1.0f;
        if (target_masked_by_alpha && sample->patch_uv_valid) {
          alpha_factor = math::clamp(
              channel_sources->scalar_at_uv(
                  PAINT_MATERIAL_CHANNEL_ALPHA, sample->patch_uv, thread_id),
              0.0f,
              1.0f);
        }
        const float alpha = math::clamp(
            math::abs(sample->value) * (sample->tex_valid ? sample->tex_color.w : 1.0f) *
                alpha_factor,
            0.0f,
            1.0f);
        if (alpha <= 0.0f) {
          continue;
        }

        const int x_px = region.x0 + i;
        const size_t coord = size_t(y_px) * ibuf.x + x_px;

        /* Precedence, matching the 3D canvas: the CHANNEL's own source wins (that is the image
         * assigned to Base Color in the channel panel); failing that the ribbon's own zone /
         * stamp texture supplies the color; failing that the flat channel or brush color. The
         * ribbon texture still contributes its intensity and alpha through `alpha` either way.
         *
         * Sampled at the ribbon's own `(u, v)` rather than through the brush mapping, which is
         * what makes a channel source follow the curve instead of standing still while the curve
         * turns under it. */
        float3 paint_rgb;
        if (target_has_source && sample->patch_uv_valid) {
          if (target.is_normal_channel) {
            /* On a flat canvas the destination tangent basis IS the image's own axes, and the
             * decal frame is the ribbon's axes laid in that same plane -- which is what turns a
             * normal decal with the curve here too. `patch_axis_*` already carry z = 0, the patch
             * being built with `plane_normal = (0, 0, 1)`. */
            paint_rgb = channel_sources->tangent_normal_packed_at_uv(
                target.channel,
                sample->patch_uv,
                thread_id,
                math::normalize(sample->patch_axis_u),
                math::normalize(sample->patch_axis_v),
                float3(0.0f, 0.0f, 1.0f),
                float3(1.0f, 0.0f, 0.0f),
                float3(0.0f, 1.0f, 0.0f));
          }
          else if (target.is_color_channel) {
            paint_rgb = channel_sources->color_at_uv(target.channel, sample->patch_uv, thread_id);
          }
          else {
            paint_rgb = float3(
                channel_sources->scalar_at_uv(target.channel, sample->patch_uv, thread_id));
          }
        }
        else if (target_paints_color) {
          /* The sampler already returns the zone / stamp texture's full RGBA at the ribbon's own
           * `(u, v)`; reading only its ALPHA painted a flat brush color and made the texture
           * invisible, which is the same defect the 3D canvas had before #curve_patch_paint_color
           * existed. Shared with that path so the two engines cannot drift on the rule: an
           * assigned texture IS the color, the Color slider does not multiply it. */
          paint_rgb = curve_patch_paint_color(
              target.flat_color, sample->tex_color, sample->tex_valid);
        }
        else {
          paint_rgb = target.flat_color;
        }

        if (target.blend == IMB_BLEND_NORMAL_MIX) {
          /* A packed tangent normal is an ENCODED direction, not a linear color. It must not be
           * pre-multiplied by coverage (scaling it toward black unpacks to a direction tilted
           * hard toward (-1, -1, -1), which reads as the whole ribbon bulging) and it must not be
           * colorspace-encoded (a normal map is data). Coverage rides in the alpha alone, which is
           * exactly what `blend_color_normal_mix_*` reads it as. Same contract the 3D canvas
           * follows -- see the NORMAL_MIX branch of `ImageColorEffect::apply_pass`. */
          if (is_float) {
            const float src_arr[4] = {paint_rgb.x, paint_rgb.y, paint_rgb.z, alpha};
            IMB_blend_color_float(
                float_data + 4 * coord, float_data + 4 * coord, src_arr, target.blend);
          }
          else {
            const float src_f[4] = {paint_rgb.x, paint_rgb.y, paint_rgb.z, alpha};
            uchar src[4];
            rgba_float_to_uchar(src, src_f);
            IMB_blend_color_byte(
                byte_data + 4 * coord, byte_data + 4 * coord, src, target.blend);
          }
        }
        else if (is_float) {
          const float4 src = image_curve_patch_blend_src_float(paint_rgb, alpha);
          const float src_arr[4] = {src.x, src.y, src.z, src.w};
          IMB_blend_color_float(
              float_data + 4 * coord, float_data + 4 * coord, src_arr, target.blend);
        }
        else {
          uchar src[4];
          image_curve_patch_blend_src_byte(paint_rgb, alpha, byte_colorspace, src);
          IMB_blend_color_byte(byte_data + 4 * coord, byte_data + 4 * coord, src, target.blend);
        }
      }
    }
  });
}

}  // namespace

void image_curve_patch_raster_draw(bContext *C, ImageCurvePatchSession &session)
{
  const CurvePatchItem &patch = session.doc.active_item();
  if (patch.geometry.spline.is_empty()) {
    return;
  }
  const CoverageBounds bounds = compute_coverage_bounds(patch.geometry);
  if (!bounds.valid) {
    return;
  }

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = paint ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush == nullptr) {
    return;
  }
  /* Direction=Subtract is this canvas's counterpart of the Sculpt viewport's Ctrl-drag: it erases
   * a channel toward its default rather than painting the slider value, and -- as there -- must
   * not be masked by an Alpha the same restamp is also erasing. */
  const bool invert = (brush->flag & BRUSH_DIR_IN) != 0;

  CurvePatchStrokeContext ctx;
  ctx.bstrength = session.params.alpha * (invert ? -1.0f : 1.0f);

  const Vector<RasterTarget> targets = resolve_targets(C, session, *paint, *brush, invert);
  if (targets.is_empty()) {
    return;
  }

  /* PBR Paint gives every channel an optional SOURCE texture of its own -- that is the image
   * assigned in the channel panel -- and an Alpha channel that masks the others. Built once for
   * the whole restamp: it is a property of the brush, and every channel's source is resolved up
   * front.
   *
   * #ChannelUvSampler rather than #ChannelSourceSampler: this canvas samples in the ribbon's own
   * `(u, v)` and has no #SculptSession to offer the brush-mapped path. */
  Scene *scene = CTX_data_scene(C);
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;
  std::optional<material::ChannelSourceSet> channel_source_set;
  std::optional<material::ChannelUvSampler> channel_sources;
  bool alpha_masking = false;
  if (!invert && brush->material_paint != nullptr &&
      paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL)
  {
    channel_source_set.emplace(
        *brush->material_paint, paint_mode, paint->visible_material_channels);
    if (channel_source_set->is_active()) {
      channel_sources.emplace(
          *channel_source_set, *brush->material_paint, paint_mode, *paint, *brush);
      alpha_masking = BKE_paint_material_channel_masks_stroke(
          *brush->material_paint, paint_mode, paint->visible_material_channels);
    }
    else {
      channel_source_set.reset();
    }
  }

  ImagePool &tex_pool = session.tex_pool_ensure();

  /* Reference-tile UV bounds, expanded to every UDIM tile they overlap (spec §9). */
  const float2 bounds_uv_min = image_curve_patch_ref_px_to_uv(
      bounds.min, session.ref_tile_uv_origin, session.ref_tile_resolution);
  const float2 bounds_uv_max = image_curve_patch_ref_px_to_uv(
      bounds.max, session.ref_tile_uv_origin, session.ref_tile_resolution);

  /* Everything above is canvas-independent -- the patch geometry and its coverage bounds are the
   * same whichever map is being written -- so it is resolved once and the pixel work below repeats
   * per canvas. One iteration for Mode=`Image`, one per enabled Principled channel for
   * Mode=`Material`.
   *
   * Unlike the 3D canvas, a channel map of a DIFFERENT resolution needs no special handling: every
   * pixel is addressed through UV, so `ref_tile_resolution` -> UV -> this tile's own resolution
   * already lands correctly. The 3D path has to skip such a channel only because it shares one
   * PBVH pixel encoding across canvases (see `curve_patch_layout_matches`). */
  for (const RasterTarget &target : targets) {
  for (ImageTile &tile : target.image->tiles) {
    const int col = (tile.tile_number - 1001) % 10;
    const int row = (tile.tile_number - 1001) / 10;
    const float2 tile_uv_origin{float(col), float(row)};
    /* AABB-vs-AABB overlap against the unit tile square. */
    if (bounds_uv_max.x < tile_uv_origin.x || bounds_uv_min.x > tile_uv_origin.x + 1.0f ||
        bounds_uv_max.y < tile_uv_origin.y || bounds_uv_min.y > tile_uv_origin.y + 1.0f)
    {
      continue;
    }

    ImageUser iuser = {};
    iuser.tile = tile.tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(target.image, &iuser, nullptr);
    if (ibuf == nullptr) {
      continue; /* Tile without a buffer is skipped, not an error (spec §16). */
    }
    const int2 tile_resolution(ibuf->x, ibuf->y);

    /* Pixel bounds within THIS tile, computed directly from tile-local UV. */
    TileRegion region;
    region.x0 = std::max(0,
                         int(std::floor((bounds_uv_min.x - tile_uv_origin.x) * float(ibuf->x))));
    region.y0 = std::max(0,
                         int(std::floor((bounds_uv_min.y - tile_uv_origin.y) * float(ibuf->y))));
    region.x1 = std::min(ibuf->x,
                         int(std::ceil((bounds_uv_max.x - tile_uv_origin.x) * float(ibuf->x))));
    region.y1 = std::min(ibuf->y,
                         int(std::ceil((bounds_uv_max.y - tile_uv_origin.y) * float(ibuf->y))));

    if (region.x1 > region.x0 && region.y1 > region.y0) {
      /* One map for every canvas: #PaintTileMap is keyed by #Image, so a channel that joins or
       * leaves the target set between restamps is still captured and restored correctly. */
      push_undo_tiles_for_region(region, *session.tiles, *target.image, *ibuf, iuser);
      blend_tile_region(region,
                        *ibuf,
                        patch,
                        session.doc.texture,
                        ctx,
                        *brush,
                        target,
                        channel_sources ? &*channel_sources : nullptr,
                        alpha_masking,
                        tex_pool,
                        tile_uv_origin,
                        tile_resolution,
                        session.ref_tile_uv_origin,
                        session.ref_tile_resolution);
      /* Deliberately NOT `ED_imapaint_dirty_region()`. Half of what that does is push the
       * "before" tiles into the IN-FLIGHT image undo step, which it reaches through
       * `ED_image_paint_tile_map_get()` -- and this session holds no such step by design: it
       * captures into its own map, two lines above, precisely so that no foreign operator can take
       * a transaction out from under a live patch (see #ED_image_paint_tile_map_new). With nothing
       * in flight that lookup returns null and the very first stamp dereferenced it.
       *
       * What is actually wanted here is the other half: mark the image dirty and tell the
       * partial-update system which pixels changed. The `imapaintpartial` rectangle
       * `ED_imapaint_dirty_region()` also accumulates belongs to `PAINT_OT_image_paint`'s stroke,
       * which clears and consumes it; a patch never does either, so feeding it only left a stale
       * dirty rect behind for the next real stroke. */
      BKE_image_mark_dirty(target.image, ibuf);
      rcti updated_region;
      BLI_rcti_init(&updated_region, region.x0, region.x1, region.y0, region.y1);
      BKE_image_partial_update_mark_region(target.image, &tile, ibuf, &updated_region);
    }

    BKE_image_release_ibuf(target.image, ibuf, nullptr);
  }
  /* Per canvas: a channel map the editor is not displaying still has to reach the viewport's
   * material preview, and the notifier carries which image changed. */
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, target.image);
  }
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Final-Quality Draw
 * \{ */

void image_curve_patch_raster_draw_final(bContext *C, ImageCurvePatchSession &session)
{
  session.frozen_patch_params.final_quality = true;
  image_curve_patch_geometry_rebuild(C, session);
  image_curve_patch_raster_draw(C, session);
}

/* \} */

}  // namespace blender::ed::sculpt_paint
