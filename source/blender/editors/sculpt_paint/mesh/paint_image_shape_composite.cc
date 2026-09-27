/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the shape compositor; see #paint_image_shape_composite.hh. The per-tile
 * backup / undo / partial-update sequence mirrors the selection gradient's, which is the
 * established pattern for one-shot full-region image writes.
 */

#include "paint_image_shape_composite.hh"

#include <algorithm>
#include <array>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_base.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "ED_image_paint_symmetry.hh"
#include "ED_paint.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_shape_raster.hh"
#include "paint_material_blend.hh"
/* #image_select_undo_session_step_get only. */
#include "paint_image_select_fragment.hh"

#include "../paint_shape_blend.hh"
#include "../paint_shape_shade.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Targets
 * \{ */

ShapeTarget shape_target_from_material_target(const PaintMaterialImageTarget &target)
{
  ShapeTarget shape_target;
  shape_target.image = target.image;
  shape_target.iuser = target.iuser;
  shape_target.channel = int(target.channel);
  /* Today the resolver only returns channel maps. Stack Layers add is_mask_target /
   * is_correction_target to #PaintMaterialImageTarget; switch on them here, and fill
   * #uv_map_name from the layer's UV. */
  shape_target.kind = ShapeTargetKind::Channel;
  shape_target.uv_map_name = "";
  shape_target.image_session_uid = target.image ? target.image->id.session_uid : 0;
  return shape_target;
}

Vector<ShapeTarget> composite_targets_get(bContext *C,
                                          Object *ob,
                                          Paint &paint,
                                          const PaintShapeSettings &settings)
{
  Vector<ShapeTarget> targets;

  Scene *scene = CTX_data_scene(C);
  ToolSettings *toolsettings = scene ? scene->toolsettings : nullptr;

  /* PBR Paint targets: the active object's material channel maps (Image Editor and 3D share
   * this when the material canvas source is active). Resolution only: the maps are created by
   * #composite_targets_ensure_writable when a draw or a session starts, so a bake never
   * allocates images as a side effect.
   *
   * Material mode NEVER falls back to the Image Editor image or the canvas image: a missing map
   * must be a visible refusal, not a silent write elsewhere. */
  if (toolsettings) {
    PaintModeSettings &mode_settings = toolsettings->paint_mode;
    if (mode_settings.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL) {
      if (ob == nullptr) {
        /* Material canvas with no active object: nothing to resolve, and no fallback to the
         * editor image. The refusal text says so. */
        return targets;
      }
      /* The channel set comes from the shared resolver, and the maps from the shared target
       * resolver, so the image backends (2D and 3D) cannot drift apart on either. `_for_channels`
       * (K1) keeps the shape's override channels, which the brush-only resolver would drop. */
      const uint32_t channels = BKE_paint_shape_target_channels(
          paint, mode_settings, settings, eShapeTargetKind::ImageMaps);
      for (const PaintMaterialImageTarget &target :
           BKE_paint_material_image_targets_get_for_channels(*ob, mode_settings, channels))
      {
        targets.append(shape_target_from_material_target(target));
      }
      return targets;
    }
  }

  /* Image canvas: in the Image Editor the tool paints on that editor's image. */
  if (SpaceImage *sima = CTX_wm_space_image(C)) {
    if (sima->image) {
      targets.append(ShapeTarget{sima->image, &sima->iuser, -1});
    }
    return targets;
  }

  if (!toolsettings || !ob) {
    return targets;
  }
  PaintModeSettings &mode_settings = toolsettings->paint_mode;
  Image *image = nullptr;
  ImageUser *iuser = nullptr;
  if (BKE_paint_canvas_image_get(&mode_settings, ob, &image, &iuser)) {
    targets.append(ShapeTarget{image, iuser, -1});
  }
  return targets;
}

const char *shape_targets_refusal_message(bContext *C, Object *ob)
{
  const Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return nullptr;
  }
  if (scene->toolsettings->paint_mode.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL) {
    return nullptr;
  }
  if (ob == nullptr) {
    return RPT_("Paint Shape: no active object for the Material canvas");
  }
  /* Stack Layers  replace this with their specific reason; the generic text matches the
   * operator's existing "no enabled image channel to paint" wording. */
  return RPT_("Paint Shape: the active material has no writable PBR channel map");
}

void composite_targets_ensure_writable(bContext *C,
                                       Object *ob,
                                       Paint &paint,
                                       const PaintShapeSettings &settings)
{
  Scene *scene = CTX_data_scene(C);
  ToolSettings *toolsettings = scene ? scene->toolsettings : nullptr;
  if (!toolsettings || !ob) {
    return;
  }
  PaintModeSettings &mode_settings = toolsettings->paint_mode;
  if (mode_settings.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL) {
    return;
  }
  if (Main *bmain = CTX_data_main(C)) {
    /* Pass the shape's channel mask (brush channels OR override channels), so override
     * channels get their maps created too. Must stay on the main thread inside the stroke's
     * undo group; the write paths never allocate. */
    const uint32_t channels = BKE_paint_shape_target_channels(
        paint, mode_settings, settings, eShapeTargetKind::ImageMaps);
    BKE_paint_material_images_ensure_writable_for_channels(*bmain, *ob, mode_settings, channels);
  }
}

/**
 * Fill the snapshot's PBR channel values from the active image-paint brush (the default source:
 * brush values, with per-part overrides only under #PAINT_SHAPE_CHANNELS_OVERRIDE). No-op in
 * override mode or without a material-paint brush: the shape's own channel values stand.
 * Strengths stay on the shape (the brush has no counterpart). Both write paths apply it - the
 * Pixel bake right before compositing, the Vector session on every style refresh - so the
 * preview shows exactly what the commit will write.
 */
void style_channels_from_brush(Paint &paint, ShapeStyle &style)
{
  if (style.use_channels_override()) {
    return;
  }
  const Brush *brush = BKE_paint_brush_for_read(&paint);
  const BrushMaterialPaint *brush_paint = brush ? brush->material_paint : nullptr;
  if (brush_paint == nullptr) {
    return;
  }
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const BrushMaterialPaintChannel &brush_channel = brush_paint->channels[i];
    for (PaintShapeChannelValue *shape_channel :
         {&style.stroke_channels[i], &style.fill_channels[i]})
    {
      shape_channel->use = brush_channel.use;
      shape_channel->blend = brush_channel.blend;
      shape_channel->value = brush_channel.value[0];
      if (i == PAINT_MATERIAL_CHANNEL_BASE_COLOR) {
        /* Synced: Base Color follows the shape's own colors (stroke -> stroke_color, fill ->
         * fill_color), so shapes keep their color independent of the brush. Unsynced: the Base
         * Color channel owns #BrushMaterialPaint.base_color for both. */
        const float4 &shape_color = (shape_channel == &style.fill_channels[i]) ? style.fill_color :
                                                                                style.stroke_color;
        float3 color;
        if (brush_paint->use_sync_base_color_with_brush) {
          color = float3(shape_color.x, shape_color.y, shape_color.z);
        }
        else {
          color = float3(brush_paint->base_color);
        }
        copy_v3_v3(shape_channel->color, color);
      }
      else if (i == PAINT_MATERIAL_CHANNEL_EMISSION) {
        copy_v3_v3(shape_channel->color, brush_channel.value);
      }
    }
  }
  if (brush_paint->use_alpha_map) {
    style.stroke_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = true;
    style.fill_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use = true;
  }
}

void style_brush_values_from_brush(Paint &paint, ShapeStyle &style)
{
  const Brush *brush = BKE_paint_brush_for_read(&paint);
  if (brush == nullptr) {
    return;
  }
  /* The colors are the shape's own (#PaintShapeSettings::stroke_color / fill_color, resolved by
   * #style_from_settings); only the strength and the canvas blend follow the active brush.
   *
   * The brush/unified Strength is the shape's overall opacity: it drives both the stroke and the
   * fill, so the header's Strength control (and Shift+F) cover every part. The compositor still
   * reads #ShapeStyle::stroke_opacity / #ShapeStyle::fill_opacity. */
  const float strength = BKE_brush_alpha_get(&paint, brush);
  style.stroke_opacity = strength;
  style.fill_opacity = strength;
  /* The canvas blend comes from the brush. PBR channels keep their own per-channel blend (see
   * #style_channels_from_brush). */
  style.stroke_blend = brush->blend;
  style.fill_blend = brush->blend;
}

static bool shape_channel_value_equal(const PaintShapeChannelValue &a,
                                      const PaintShapeChannelValue &b)
{
  return a.use == b.use && a.blend == b.blend && a.value == b.value && a.strength == b.strength &&
         a.color[0] == b.color[0] && a.color[1] == b.color[1] && a.color[2] == b.color[2];
}

bool shape_brush_style_equal(const ShapeStyle &a, const ShapeStyle &b)
{
  if (a.stroke_opacity != b.stroke_opacity || a.fill_opacity != b.fill_opacity ||
      a.stroke_blend != b.stroke_blend || a.fill_blend != b.fill_blend)
  {
    return false;
  }
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!shape_channel_value_equal(a.stroke_channels[i], b.stroke_channels[i]) ||
        !shape_channel_value_equal(a.fill_channels[i], b.fill_channels[i]))
    {
      return false;
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile helpers
 * \{ */

static ImageUser tile_iuser_get(const ShapeTarget &target, const int tile_number)
{
  ImageUser iuser;
  if (target.iuser) {
    iuser = *target.iuser;
  }
  iuser.tile = tile_number;
  return iuser;
}

/** Expand \a r_region to also cover \a other (both half-open tile rects, either may be empty). */
void shape_region_union(rcti &r_region, const rcti &other)
{
  if (BLI_rcti_is_empty(&other)) {
    return;
  }
  if (BLI_rcti_is_empty(&r_region)) {
    r_region = other;
    return;
  }
  BLI_rcti_union(&r_region, &other);
}

/** Selection-mask bounds grown by the mask's own feather, so the AA fringe survives. */
static rcti selection_bounds_rect(Image *image,
                                  const int tile_number,
                                  const int tile_w,
                                  const int tile_h)
{
  int sel_min[2], sel_max[2];
  if (!BKE_image_paint_selection_mask_bounds(image, tile_number, sel_min, sel_max)) {
    return rcti{0, 0, 0, 0};
  }
  const int feather = BKE_image_paint_selection_edge_policy_feathered().blend_radius_px + 1;
  return rcti{std::max(0, sel_min[0] - feather),
              std::min(tile_w, sel_max[0] + 1 + feather),
              std::max(0, sel_min[1] - feather),
              std::min(tile_h, sel_max[1] + 1 + feather)};
}

/**
 * Tile-local pixel rect covered by \a shapes on one tile: the union of every shape's bounds
 * region, clipped to the tile and the selection mask. Returns false when nothing is covered.
 *
 * The shapes are defined in reference-tile pixels but rasterize into this tile at the tile's own
 * resolution (PBR channel maps commonly differ in size from the canvas): when it differs, the
 * geometry and the pixel-based style widths scale first, so the tile offset below maps whole
 * tiles into target pixels (a scaled reference tile is exactly the target tile size).
 */
static bool tile_region_calc(Span<PaintShape> shapes,
                             const CanvasTile &tile,
                             const ShapeStyle &style,
                             const int tile_number,
                             const int tile_w,
                             const int tile_h,
                             const bool use_selection_mask,
                             Image *image,
                             rcti &r_region)
{
  BLI_rcti_init(&r_region, 0, 0, 0, 0);

  const float2 scale = float2(float(tile_w), float(tile_h)) / float2(tile.ref_tile_size);
  const bool needs_scale = (scale.x != 1.0f) || (scale.y != 1.0f);
  Vector<PaintShape> scaled_shapes;
  ShapeStyle scaled_style;
  if (needs_scale) {
    scaled_shapes = shapes_scale_to_target(shapes, scale);
    scaled_style = style_scale_to_target(style, scale);
  }
  const Span<PaintShape> region_shapes = needs_scale ? scaled_shapes.as_span() : shapes;
  const ShapeStyle &region_style = needs_scale ? scaled_style : style;

  const float2 ref_origin = tile_uv_origin(tile.ref_tile);
  const float2 tile_origin = tile_uv_origin(tile_number);
  /* Shape-space pixel position of this tile's (0, 0) corner. */
  const float2 tile_offset_px = (tile_origin - ref_origin) * float2(float(tile_w), float(tile_h));

  rcti bounds_region;
  BLI_rcti_init(&bounds_region, 0, 0, 0, 0);
  for (const PaintShape &shape : region_shapes) {
    const rctf shape_bounds = shape_bounds_calc(shape, region_style);
    if (shape_bounds.xmin > shape_bounds.xmax) {
      continue;
    }
    rcti part;
    BLI_rcti_init(&part,
                  int(std::floor(shape_bounds.xmin - tile_offset_px.x)),
                  int(std::ceil(shape_bounds.xmax - tile_offset_px.x)),
                  int(std::floor(shape_bounds.ymin - tile_offset_px.y)),
                  int(std::ceil(shape_bounds.ymax - tile_offset_px.y)));
    shape_region_union(bounds_region, part);
  }
  if (BLI_rcti_is_empty(&bounds_region)) {
    return false;
  }

  rcti region = bounds_region;
  if (use_selection_mask && !BKE_image_paint_selection_derived_active(image)) {
    const rcti sel_rect = selection_bounds_rect(image, tile_number, tile_w, tile_h);
    if (BLI_rcti_is_empty(&sel_rect)) {
      return false;
    }
    BLI_rcti_isect(&region, &sel_rect, &region);
  }

  rcti tile_rect;
  BLI_rcti_init(&tile_rect, 0, tile_w, 0, tile_h);
  BLI_rcti_isect(&region, &tile_rect, &region);
  if (BLI_rcti_is_empty(&region)) {
    return false;
  }
  r_region = region;
  return true;
}

rcti composite_tile_region_get(Image *image,
                               const ImageUser *owner_iuser,
                               const int tile_number,
                               const Span<PaintShape> shapes,
                               const CanvasTile &tile,
                               const ShapeStyle &style)
{
  rcti region;
  BLI_rcti_init(&region, 0, 0, 0, 0);
  if (!image || shapes.is_empty()) {
    return region;
  }
  const ShapeTarget target{image, const_cast<ImageUser *>(owner_iuser), -1};
  ImageUser iuser = tile_iuser_get(target, tile_number);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (!ibuf) {
    return region;
  }
  tile_region_calc(shapes,
                   tile,
                   style,
                   tile_number,
                   ibuf->x,
                   ibuf->y,
                   BKE_image_paint_selection_is_active(image),
                   image,
                   region);
  BKE_image_release_ibuf(image, ibuf, lock);
  return region;
}

Vector<int> composite_affected_tiles_get(Image *image,
                                         const ImageUser *owner_iuser,
                                         const Span<PaintShape> shapes,
                                         const CanvasTile &tile,
                                         const ShapeStyle &style)
{
  Vector<int> tiles;
  if (!image || shapes.is_empty()) {
    return tiles;
  }
  const ShapeTarget target{image, const_cast<ImageUser *>(owner_iuser), -1};

  for (const ImageTile *itile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = tile_iuser_get(target, itile->tile_number);
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
    if (!ibuf) {
      continue;
    }
    rcti region;
    const bool affected = tile_region_calc(shapes,
                                           tile,
                                           style,
                                           itile->tile_number,
                                           ibuf->x,
                                           ibuf->y,
                                           BKE_image_paint_selection_is_active(image),
                                           image,
                                           region);
    BKE_image_release_ibuf(image, ibuf, lock);
    if (affected) {
      tiles.append(itile->tile_number);
    }
  }
  return tiles;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Tile backups
 * \{ */

ImBuf *shape_tile_backup_init(Image *image, const ImageUser *owner_iuser, int tile_number)
{
  if (!image) {
    return nullptr;
  }
  const ShapeTarget target{image, const_cast<ImageUser *>(owner_iuser), -1};
  ImageUser iuser = tile_iuser_get(target, tile_number);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (!ibuf) {
    return nullptr;
  }
  ImBuf *backup = IMB_dupImBuf(ibuf);
  BKE_image_release_ibuf(image, ibuf, lock);
  return backup;
}

/** Copy \a region (half-open, tile pixels) of \a backup into the tile buffer. */
static void backup_copy_region(ImBuf *ibuf, const ImBuf &backup, const rcti &region)
{
  BLI_assert(ibuf->x == backup.x && ibuf->y == backup.y);
  if (BLI_rcti_is_empty(&region) || ibuf->x != backup.x || ibuf->y != backup.y) {
    return;
  }
  const int2 pos(region.xmin, region.ymin);
  const int2 size(region.xmax - region.xmin, region.ymax - region.ymin);
  if (ibuf->float_buffer.data && backup.float_buffer.data) {
    IMB_copy_rect(ibuf, &backup, pos, pos, size);
  }
  else if (ibuf->byte_buffer.data && backup.byte_buffer.data) {
    IMB_copy_rect(ibuf, &backup, pos, pos, size);
  }
}

/** Mark a restored / written region for redraw. Safe without a context (uses the global
 * notifier, like the RNA update paths). \a commit also flags the image as modified; the live
 * Vector preview and the restore paths must not, or a cancelled session would leave the image
 * looking unsaved. */
static void tile_mark_region(Image *image,
                             const ImageUser *owner_iuser,
                             const int tile_number,
                             const rcti &region,
                             const bool commit)
{
  ImageUser iuser;
  if (owner_iuser) {
    iuser = *owner_iuser;
  }
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (!ibuf) {
    return;
  }
  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  const ImageTile *itile = BKE_image_get_tile(image, tile_number);
  BKE_image_partial_update_mark_region(image, itile, ibuf, &region);
  if (commit) {
    BKE_image_mark_dirty(image, ibuf);
  }
  BKE_image_release_ibuf(image, ibuf, lock);
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, image);
}

static void tile_restore(Image *image,
                         const ImageUser *owner_iuser,
                         const int tile_number,
                         const ImBuf &backup,
                         const rcti &region)
{
  if (!image || BLI_rcti_is_empty(&region)) {
    return;
  }
  ImageUser iuser;
  if (owner_iuser) {
    iuser = *owner_iuser;
  }
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (!ibuf) {
    return;
  }
  backup_copy_region(ibuf, backup, region);
  BKE_image_release_ibuf(image, ibuf, lock);
  DEG_id_tag_update(&image->id, 0);
  tile_mark_region(image, owner_iuser, tile_number, region, false);
}

void shape_tile_backup_restore(Image *image,
                               const ImageUser *owner_iuser,
                               const int tile_number,
                               const ImBuf &backup)
{
  if (!image) {
    return;
  }
  ImageUser iuser;
  if (owner_iuser) {
    iuser = *owner_iuser;
  }
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  if (!ibuf) {
    return;
  }
  const rcti whole{0, ibuf->x, 0, ibuf->y};
  BKE_image_release_ibuf(image, ibuf, lock);
  tile_restore(image, owner_iuser, tile_number, backup, whole);
}

void shape_tile_backup_restore_region(Image *image,
                                      const ImageUser *owner_iuser,
                                      const int tile_number,
                                      const ImBuf &backup,
                                      const rcti &region)
{
  tile_restore(image, owner_iuser, tile_number, backup, region);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compositing
 * \{ */

/** One tile being written, with the buffers acquired once up front. */
struct CompositeTile {
  int tile_number = 0;
  ImBuf *ibuf = nullptr;
  float *float_data = nullptr;
  uchar *byte_data = nullptr;
  const ColorSpace *byte_colorspace = nullptr;
  bool byte_is_data = false;
  /** Tile-local pixel region the coverage buffers span. */
  rcti region;
};

/** Load a tile pixel into the tile's working space. The byte path is a plain normalization: the
 * style was pre-converted into the buffer's colorspace once per tile (see the write loop), so the
 * bytes and the style already share a space. */
static float4 composite_pixel_load(const CompositeTile &tile, const int px, const int y)
{
  if (tile.float_data) {
    const float *dst_px = tile.float_data + 4 * (size_t(y) * tile.ibuf->x) + 4 * px;
    return float4(dst_px[0], dst_px[1], dst_px[2], dst_px[3]);
  }
  const uchar *dst_px = tile.byte_data + 4 * (size_t(y) * tile.ibuf->x) + 4 * px;
  float dst_float[4];
  rgba_uchar_to_float(dst_float, dst_px);
  return float4(dst_float[0], dst_float[1], dst_float[2], dst_float[3]);
}

static void composite_pixel_store(CompositeTile &tile,
                                  const int px,
                                  const int y,
                                  const float4 &color)
{
  if (tile.float_data) {
    float *dst_px = tile.float_data + 4 * (size_t(y) * tile.ibuf->x) + 4 * px;
    dst_px[0] = color.x;
    dst_px[1] = color.y;
    dst_px[2] = color.z;
    dst_px[3] = color.w;
    return;
  }
  uchar *dst_px = tile.byte_data + 4 * (size_t(y) * tile.ibuf->x) + 4 * px;
  const float dst_float[4] = {color.x, color.y, color.z, color.w};
  rgba_float_to_uchar(dst_px, dst_float);
}

/** The part color to write with: scene linear normally, the picked sRGB value for data
 * (non-color) byte images where no color management may reinterpret the value. */
static float4 part_color(const ShapeStyle &style, const bool is_stroke, const CompositeTile &tile)
{
  if (tile.byte_data && tile.byte_is_data) {
    return is_stroke ? style.stroke_color_picked : style.fill_color_picked;
  }
  return is_stroke ? style.stroke_color : style.fill_color;
}

/**
 * Composite one strip of \a tile from the coverage of \a cov through the shared
 * #shape_blend_pixel core. \a ctx is built once per tile and channel by the caller (its style and
 * alpha_active are constant across the tile); \a gradient is the fill bounding box for the
 * gradient fill path, whose coverage was rasterized from one shape alone (null otherwise).
 * \a tile_offset_px maps tile pixels to shape-space positions.
 */
static void tile_composite_pixels(CompositeTile &tile,
                                  const ShapeCoverage &cov,
                                  const ShapeBlendContext &ctx,
                                  const float2 &tile_offset_px,
                                  const ShapeFillGradient *gradient,
                                  Image *image)
{
  const bool use_selection_mask = BKE_image_paint_selection_is_active(image);
  /* The rasterizer only allocates the requested outputs, so the optional distance/direction
   * buffers may be absent; the shader treats a missing output as zero. */
  const bool has_fill_d = !cov.fill_d.is_empty();
  const bool has_stroke_dir = !cov.stroke_dir.is_empty();
  const bool has_fill_dir = !cov.fill_dir.is_empty();

  threading::parallel_for(
      IndexRange(cov.rect.ymin, cov.rect.ymax - cov.rect.ymin),
      32,
      [&](const IndexRange rows) {
        for (const int y : rows) {
          for (const int px : IndexRange(cov.rect.xmin, cov.rect.xmax - cov.rect.xmin)) {
            const int64_t idx = cov.index(px, y);

            float sel_w = 1.0f;
            if (use_selection_mask) {
              sel_w = BKE_image_paint_selection_blend_sample_bilinear(
                  image, tile.tile_number, float(px) + 0.5f, float(y) + 0.5f);
              if (sel_w <= 0.0f) {
                continue;
              }
            }

            ShapeSample sample;
            sample.fill = cov.fill[idx];
            sample.stroke = cov.stroke[idx];
            sample.stroke_t = cov.stroke_t[idx];
            sample.fill_d = has_fill_d ? cov.fill_d[idx] : 0.0f;
            sample.stroke_dir = has_stroke_dir ? cov.stroke_dir[idx] : float2(0.0f);
            sample.fill_dir = has_fill_dir ? cov.fill_dir[idx] : float2(0.0f);
            if (sample.fill <= 0.0f && sample.stroke <= 0.0f) {
              continue;
            }

            const float2 p = tile_offset_px + float2(float(px) + 0.5f, float(y) + 0.5f);
            ShapeBlendContext pixel_ctx = ctx;
            /* Only the canvas path shades a gradient; the PBR channels never carry one. */
            pixel_ctx.fill_gradient = (ctx.channel < 0) ? gradient : nullptr;

            float4 dst = composite_pixel_load(tile, px, y);
            shape_blend_pixel(dst, pixel_ctx, sample, sel_w, p);
            composite_pixel_store(tile, px, y, dst);
          }
        }
      });
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Composite into explicit targets
 * \{ */

bool shape_composite_into(const Span<ShapeTarget> targets,
                          const Span<PaintShape> shapes,
                          const CanvasTile &tile,
                          const ShapeStyle &style_in,
                          const bool push_undo,
                          const char *undo_name,
                          Object *ob)
{
  if (shapes.is_empty()) {
    return false;
  }
  /* An open shape draws only through its stroke; force it on (see #style_resolve_for_shapes) so
   * the 2D bake matches the 3D backends for a Line / Polyline / open Curve. */
  const ShapeStyle style = style_resolve_for_shapes(shapes, style_in);
  /* \a shapes are already symmetry-expanded (see #shape_bake and the Vector session); all tiles
   * and regions below are derived from them, so copies outside the original's bounds are never
   * cut. */
  /* Whether Alpha masks the other channels is a property of the bake, not of a tile; the shared
   * blend core reads it from the context instead of recomputing it per pixel. */
  const bool alpha_active = (style.stroke_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use != 0) ||
                            (style.fill_channels[PAINT_MATERIAL_CHANNEL_ALPHA].use != 0);

  struct TileWrite {
    ShapeTarget target;
    int tile_number;
    ImBuf *backup;
    rcti region;
    /** Pixel size of this tile (channel maps of one material may differ from each other). */
    int2 tile_size;
  };
  Vector<TileWrite> writes;

  for (const ShapeTarget &target : targets) {
    if (!target.image) {
      continue;
    }
    const bool use_mask = BKE_image_paint_selection_is_active(target.image);
    for (const ImageTile *itile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ImageUser iuser = tile_iuser_get(target, itile->tile_number);
      void *lock = nullptr;
      ImBuf *ibuf = BKE_image_acquire_ibuf(target.image, &iuser, &lock);
      if (!ibuf) {
        continue;
      }
      rcti region;
      const bool affected = tile_region_calc(shapes,
                                             tile,
                                             style,
                                             itile->tile_number,
                                             ibuf->x,
                                             ibuf->y,
                                             use_mask,
                                             target.image,
                                             region);
      BKE_image_release_ibuf(target.image, ibuf, lock);
      if (!affected) {
        continue;
      }
      /* The backups only feed the undo step; the preview path (the Vector session) keeps its
       * own long-lived ones and skips this per-write duplicate. */
      ImBuf *backup = push_undo ?
                          shape_tile_backup_init(target.image, target.iuser, itile->tile_number) :
                          nullptr;
      writes.append(TileWrite{target, itile->tile_number, backup, region, int2(ibuf->x, ibuf->y)});
    }
  }
  if (writes.is_empty()) {
    return false;
  }

  /* Write pass: rasterize + composite in row strips, so peak memory stays at
   * strip_size x region_width instead of the whole region. Gradient fills composite per shape
   * (each with its own fill bounding box); everything else takes the combined union path. */
  constexpr int STRIP_ROWS = 128;
  const bool gradient_fill = style.use_fill() && style.fill_type == PAINT_SHAPE_FILL_GRADIENT &&
                             style.use_fill_gradient;

  /* One write's buffer held across the strip loop, with its per-tile working style (the
   * colorspace conversions are constant per tile, so they are baked in once in the write loop
   * instead of per pixel). Data (non-color) byte images keep the picked values as-is (see
   * #part_color). */
  struct AcquiredWrite {
    TileWrite *write = nullptr;
    ImBuf *ibuf = nullptr;
    void *lock = nullptr;
    CompositeTile tile;
    /** The style copy with the tile-converted colors (and, for PBR color channels, the
     * tile-converted channel entry), built in the write loop from the scaled style. */
    ShapeStyle work_style;
    /** Core blend context, pointed at #work_style; built with the style in the write loop. */
    ShapeBlendContext ctx;
  };
  auto acquired_write_init = [&](TileWrite &member_write, AcquiredWrite &r_member) -> bool {
    ImageUser iuser = tile_iuser_get(member_write.target, member_write.tile_number);
    r_member.lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(member_write.target.image, &iuser, &r_member.lock);
    if (!ibuf) {
      return false;
    }
    r_member.write = &member_write;
    r_member.ibuf = ibuf;
    r_member.tile.tile_number = member_write.tile_number;
    r_member.tile.ibuf = ibuf;
    r_member.tile.region = member_write.region;
    if (ibuf->float_buffer.data) {
      r_member.tile.float_data = ibuf->float_data_for_write();
    }
    else if (ibuf->byte_buffer.data) {
      r_member.tile.byte_data = ibuf->byte_data_for_write();
      r_member.tile.byte_colorspace = ibuf->byte_buffer.colorspace;
      r_member.tile.byte_is_data = r_member.tile.byte_colorspace &&
                                   IMB_colormanagement_space_is_data(r_member.tile.byte_colorspace);
    }
    return true;
  };

  Vector<bool> done(writes.size(), false);
  for (int write_index = 0; write_index < writes.size(); write_index++) {
    if (done[write_index]) {
      continue;
    }
    done[write_index] = true;
    TileWrite &write = writes[write_index];
    const bool is_pbr = write.target.channel >= 0;

    /* Acquire the primary tile and, for a PBR channel target, every not-yet-written channel
     * target with the same tile geometry: an identical (tile number, region) means the
     * identical coverage, so the strips below rasterize once and composite into all of them (N
     * channel targets used to rasterize N times). */
    Vector<AcquiredWrite> acquired;
    acquired.append(AcquiredWrite());
    if (!acquired_write_init(write, acquired.last())) {
      acquired.clear();
    }
    if (is_pbr && !acquired.is_empty()) {
      for (int other_index = write_index + 1; other_index < writes.size(); other_index++) {
        if (done[other_index] || writes[other_index].target.channel < 0) {
          continue;
        }
        const rcti &other_region = writes[other_index].region;
        if (writes[other_index].tile_number != write.tile_number ||
            writes[other_index].tile_size != write.tile_size ||
            other_region.xmin != write.region.xmin || other_region.xmax != write.region.xmax ||
            other_region.ymin != write.region.ymin || other_region.ymax != write.region.ymax)
        {
          continue;
        }
        acquired.append(AcquiredWrite());
        if (!acquired_write_init(writes[other_index], acquired.last())) {
          acquired.remove_last();
          continue;
        }
        done[other_index] = true;
      }
    }
    if (acquired.is_empty()) {
      continue;
    }

    /* The shapes rasterize into this tile at the tile's own resolution (PBR channel maps of one
     * material may differ from each other and from the canvas): scale the geometry and the
     * pixel-based style widths when the tile differs from the reference tile. A scaled reference
     * tile is exactly this tile's size, so the offset below maps whole tiles into target pixels. */
    const float2 scale = float2(write.tile_size) / float2(tile.ref_tile_size);
    const bool needs_scale = (scale.x != 1.0f) || (scale.y != 1.0f);
    Vector<PaintShape> scaled_shapes;
    ShapeStyle scaled_style;
    if (needs_scale) {
      scaled_shapes = shapes_scale_to_target(shapes, scale);
      scaled_style = style_scale_to_target(style, scale);
    }
    const Span<PaintShape> raster_shapes = needs_scale ? scaled_shapes.as_span() : shapes;
    const ShapeStyle &raster_style = needs_scale ? scaled_style : style;

    const float2 ref_origin = tile_uv_origin(tile.ref_tile);
    const float2 tile_origin = tile_uv_origin(write.tile_number);
    const float2 tile_offset_px = (tile_origin - ref_origin) * float2(write.tile_size);

    /* Per-tile working styles: the scaled style with the buffer's color space baked into the
     * part colors and the ramp tables once (data (non-color) byte images keep the picked
     * values, see #part_color); PBR color channels additionally pre-convert their channel
     * entry. */
    for (AcquiredWrite &member : acquired) {
      member.work_style = raster_style;
      member.work_style.stroke_color = part_color(style, true, member.tile);
      member.work_style.fill_color = part_color(style, false, member.tile);
      if (member.tile.byte_data && member.tile.byte_colorspace && !member.tile.byte_is_data) {
        IMB_colormanagement_scene_linear_to_colorspace_v3(member.work_style.stroke_color,
                                                          member.tile.byte_colorspace);
        IMB_colormanagement_scene_linear_to_colorspace_v3(member.work_style.fill_color,
                                                          member.tile.byte_colorspace);
        for (float4 &ramp_color : member.work_style.stroke_ramp_table) {
          IMB_colormanagement_scene_linear_to_colorspace_v3(ramp_color,
                                                            member.tile.byte_colorspace);
        }
        for (float4 &gradient_color : member.work_style.fill_gradient_table) {
          IMB_colormanagement_scene_linear_to_colorspace_v3(gradient_color,
                                                            member.tile.byte_colorspace);
        }
      }
    }
    if (is_pbr) {
      for (AcquiredWrite &member : acquired) {
        if (member.tile.byte_data && member.tile.byte_colorspace &&
            !member.tile.byte_is_data &&
            ELEM(member.write->target.channel,
                 PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                 PAINT_MATERIAL_CHANNEL_EMISSION))
        {
          for (PaintShapeChannelValue *entry :
               {&member.work_style.stroke_channels[member.write->target.channel],
                &member.work_style.fill_channels[member.write->target.channel]})
          {
            float color[3] = {entry->color[0], entry->color[1], entry->color[2]};
            IMB_colormanagement_scene_linear_to_colorspace_v3(color, member.tile.byte_colorspace);
            copy_v3_v3(entry->color, color);
          }
        }
      }
    }
    /* Bind each member's context to its now-final working style. */
    for (AcquiredWrite &member : acquired) {
      member.ctx.style = &member.work_style;
      member.ctx.channel = member.write->target.channel;
      member.ctx.alpha_active = alpha_active;
    }

    CompositeTile &tile = acquired[0].tile;
    if (is_pbr && (tile.float_data || tile.byte_data)) {
      /* The channel shader reads the base coverage plus the fill distance for profiled fills;
       * the direction/distance buffers stay empty unless a Normal or Height target shares the
       * tile geometry. */
      ShapeRasterOutputs outputs = ShapeRasterOutputs::Fill | ShapeRasterOutputs::Stroke |
                                   ShapeRasterOutputs::StrokeT;
      if (raster_style.use_fill() && raster_style.profile_affects_coverage()) {
        outputs |= ShapeRasterOutputs::FillD;
      }
      bool need_shape_directions = false;
      for (const AcquiredWrite &member : acquired) {
        if (ELEM(member.write->target.channel,
                 PAINT_MATERIAL_CHANNEL_HEIGHT,
                 PAINT_MATERIAL_CHANNEL_NORMAL))
        {
          need_shape_directions = true;
          break;
        }
      }
      if (need_shape_directions) {
        outputs |= ShapeRasterOutputs::StrokeDir | ShapeRasterOutputs::FillDir |
                   ShapeRasterOutputs::FillD;
      }
      ShapeRasterizer rasterizer(
          raster_shapes, raster_style, write.region, tile_offset_px, outputs);
      for (int y0 = write.region.ymin; y0 < write.region.ymax; y0 += STRIP_ROWS) {
        rcti strip;
        BLI_rcti_init(&strip,
                      write.region.xmin,
                      write.region.xmax,
                      y0,
                      std::min(y0 + STRIP_ROWS, write.region.ymax));
        const ShapeCoverage coverage = rasterizer.rasterize(strip);
        for (AcquiredWrite &member : acquired) {
          member.tile.region = strip;
          tile_composite_pixels(
              member.tile, coverage, member.ctx, tile_offset_px, nullptr, member.write->target.image);
        }
      }
    }
    else if (tile.float_data || tile.byte_data) {
      /* Canvas shading reads the base coverage plus the fill distance for profiled fills; the
       * direction buffers stay empty. */
      ShapeRasterOutputs canvas_outputs = ShapeRasterOutputs::Fill | ShapeRasterOutputs::Stroke |
                                          ShapeRasterOutputs::StrokeT;
      if (raster_style.use_fill() && raster_style.profile_affects_coverage()) {
        canvas_outputs |= ShapeRasterOutputs::FillD;
      }
      auto composite_strips = [&](const Span<PaintShape> pass_shapes,
                                  const ShapeFillGradient *gradient) {
        ShapeRasterizer rasterizer(
            pass_shapes, raster_style, write.region, tile_offset_px, canvas_outputs);
        for (int y0 = write.region.ymin; y0 < write.region.ymax; y0 += STRIP_ROWS) {
          rcti strip;
          BLI_rcti_init(&strip,
                        write.region.xmin,
                        write.region.xmax,
                        y0,
                        std::min(y0 + STRIP_ROWS, write.region.ymax));
          const ShapeCoverage coverage = rasterizer.rasterize(strip);
          tile.region = strip;
          tile_composite_pixels(
              tile, coverage, acquired[0].ctx, tile_offset_px, gradient, write.target.image);
        }
      };

      if (gradient_fill) {
        for (const PaintShape &shape : raster_shapes) {
          const rctf bbox = shape_bounds_calc(shape, raster_style);
          const ShapeFillGradient gradient{float2(bbox.xmin, bbox.ymin),
                                           float2(bbox.xmax, bbox.ymax)};
          composite_strips(Span<PaintShape>(&shape, 1), &gradient);
        }
      }
      else {
        composite_strips(raster_shapes, nullptr);
      }
    }

    for (AcquiredWrite &member : acquired) {
      if (member.tile.float_data || member.tile.byte_data) {
        member.tile.ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
        if (push_undo) {
          BKE_image_mark_dirty(member.write->target.image, member.tile.ibuf);
        }
      }
      BKE_image_release_ibuf(member.write->target.image, member.tile.ibuf, member.lock);
    }
  }

  /* One undo step for everything written; the undo buffers are the pre-write backups, pushed
   * after the write like the selection gradient does. Only open a step when at least one tile
   * actually produced a backup, so a failed backup never leaves an unmatched push_end. */
  if (push_undo) {
    ImageUndoStep *us_open = nullptr;
    bool undo_open = false;
    for (const TileWrite &write : writes) {
      if (!write.backup) {
        continue;
      }
      ImageUser iuser = tile_iuser_get(write.target, write.tile_number);
      if (!undo_open) {
        ED_image_undo_push_begin_with_image(undo_name, write.target.image, write.backup, &iuser);
        us_open = image_select_undo_session_step_get();
        undo_open = true;
      }
      else if (us_open) {
        ED_image_undo_push(write.target.image, write.backup, &iuser, us_open);
      }
      ED_image_undo_capture_selection_mask(write.target.image, write.tile_number);
    }
    if (undo_open) {
      ED_image_undo_push_end();
    }
  }

  /* Partial updates for exactly the written regions. The live preview relies on the
   * partial-update register alone (fast, like the 2D paint path); the committed bake also
   * releases the cached GPU textures so the Image Editor rebuilds them from the final ImBufs on
   * the next redraw -- the pattern of every other pixel-writing tool in this fork (see
   * paint_image_select_gradient.cc). */
  for (const TileWrite &write : writes) {
    tile_mark_region(
        write.target.image, write.target.iuser, write.tile_number, write.region, push_undo);
  }
  for (const ShapeTarget &target : targets) {
    if (!target.image) {
      continue;
    }
    if (push_undo) {
      BKE_image_free_gputextures(target.image);
    }
    if (target.channel >= 0) {
      /* Material maps feed the 3D Viewport shading. The committed bake tags the shading graph
       * here; the Vector preview tags its images with a throttle of its own (see the Vector
       * session's refresh), so a drag does not re-evaluate the shading per mouse move. */
      if (push_undo) {
        DEG_id_tag_update(&target.image->id, ID_RECALC_SHADING | ID_RECALC_PARAMETERS);
      }
    }
    else {
      DEG_id_tag_update(&target.image->id, push_undo ? 0 : ID_RECALC_EDITORS);
    }
    /* The 2D paint path sends NA_PAINTING during the stroke and NA_EDITED at its end. */
    WM_main_add_notifier(NC_IMAGE | (push_undo ? NA_EDITED : NA_PAINTING), target.image);
  }
  if (push_undo && ob && ob->type == OB_MESH) {
    /* The committed material maps need a shading refresh on the active object's material, which
     * the image tags above alone do not reach. */
    DEG_id_tag_update(&ob->id, ID_RECALC_SHADING);
  }

  for (TileWrite &write : writes) {
    IMB_freeImBuf(write.backup);
    write.backup = nullptr;
  }

  return true;
}

bool shape_bake(bContext *C,
                const Span<PaintShape> shapes,
                const CanvasTile &tile,
                const ShapeStyle &style,
                const bool push_undo,
                const char *undo_name)
{
  if (shapes.is_empty()) {
    return false;
  }
  Scene *scene = CTX_data_scene(C);
  if (!scene || !scene->toolsettings) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  /* The Image Editor draws into the image-paint channels. */
  Paint &paint = scene->toolsettings->imapaint.paint;
  const Vector<ShapeTarget> targets = composite_targets_get(
      C, ob, paint, BKE_paint_shape_settings_get(*scene->toolsettings));
  if (targets.is_empty()) {
    return false;
  }

  /* PBR channel values default to the active brush (see #style_channels_from_brush). */
  ShapeStyle bake_style = style;
  style_channels_from_brush(paint, bake_style);

  /* Symmetry copies of every shape (the originals included). */
  const Vector<PaintShape> all_shapes = shapes_expand_symmetry(shapes, tile, *scene->toolsettings);
  /* The object tag for the material maps is #shape_composite_into's commit-time job. */
  return shape_composite_into(targets, all_shapes, tile, bake_style, push_undo, undo_name, ob);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image tile write target
 * \{ */

ImageTilesBackend::ImageTilesBackend(const PaintShapeSettings &settings, const CanvasTile &tile)
    : settings_(settings), tile_(tile)
{
}

ImageTilesBackend::~ImageTilesBackend()
{
  for (TileBackup &backup : backups_) {
    if (backup.orig) {
      IMB_freeImBuf(backup.orig);
      backup.orig = nullptr;
    }
  }
}

bool ImageTilesBackend::begin(bContext &C, ReportList *reports)
{
  Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  Object *ob = CTX_data_active_object(&C);
  /* The one map-creation call of the session runs before the resolution, so a preview write
   * never allocates images (it would run outside any undo step that records the creation). */
  Paint &paint = scene->toolsettings->imapaint.paint;
  composite_targets_ensure_writable(&C, ob, paint, settings_);
  targets_ = composite_targets_get(&C, ob, paint, settings_);
  if (targets_.is_empty()) {
    /* No fallback to the editor image: in Material mode an empty resolution is a refusal, and
     * the reason is reported to the caller. Image canvases always resolve an image. */
    if (const char *reason = shape_targets_refusal_message(&C, ob)) {
      if (reports != nullptr) {
        BKE_report(reports, RPT_WARNING, reason);
      }
    }
    return false;
  }
  /* Own the ImageUsers by value: a material-node cache the resolution pointed into can be freed
   * by a material edit or an undo while the session is live. The vector is reserved to its final
   * size first, so the pointers fixed below stay stable. The session_uid snapshot is what lets a
   * later write tell whether the Image is still the one this session resolved. */
  target_iusers_.reserve(targets_.size());
  for (ShapeTarget &target : targets_) {
    target.image_session_uid = target.image ? target.image->id.session_uid : 0;
    target_iusers_.append(target.iuser ? *target.iuser : ImageUser());
  }
  for (const int i : targets_.index_range()) {
    targets_[i].iuser = &target_iusers_[i];
  }
  ob_ = ob;
  /* Keep the Main so a later liveness check can look the Image up by session_uid without ever
   * touching the (possibly freed) stored pointer. */
  bmain_ = CTX_data_main(&C);
  return true;
}

bool ImageTilesBackend::target_alive(const ShapeTarget &target) const
{
  if (target.image == nullptr) {
    return true;
  }
  if (bmain_ == nullptr) {
    /* No Main to validate against (a test backend): trust the resolution. */
    return true;
  }
  /* The stored `target.image` may already be dangling, so never dereference it: find the ID that
   * currently owns this session_uid and compare its address with the stored pointer (IDs are
   * unique per session, and the freed Image's uid is not reused). */
  ID *found = BKE_libblock_find_session_uid(bmain_, ID_IM, target.image_session_uid);
  return found == reinterpret_cast<ID *>(target.image);
}

bool ImageTilesBackend::targets_alive() const
{
  for (const ShapeTarget &target : targets_) {
    if (!this->target_alive(target)) {
      return false;
    }
  }
  return true;
}

ImageTilesBackend::TileBackup *ImageTilesBackend::backups_find(const int target_index,
                                                               const int tile_number)
{
  for (TileBackup &backup : backups_) {
    if (backup.target_index == target_index && backup.tile_number == tile_number) {
      return &backup;
    }
  }
  return nullptr;
}

bool ImageTilesBackend::preview(bContext * /*C*/,
                                ReportList * /*reports*/,
                                const Span<PaintShape> shapes,
                                const ShapeStyle &style)
{
  if (targets_.is_empty() || shapes.is_empty()) {
    return false;
  }
  /* A target Image freed by an undo / material edit must not be written through. */
  if (!this->targets_alive()) {
    return false;
  }

  /* Lazy backups: tiles the shape moved onto are backed up before the first write into them,
   * per target (each channel image is backed up on its own). */
  for (const int target_index : targets_.index_range()) {
    const ShapeTarget &target = targets_[target_index];
    const Vector<int> affected = composite_affected_tiles_get(
        target.image, target.iuser, shapes, tile_, style);
    for (const int tile_number : affected) {
      if (backups_find(target_index, tile_number)) {
        continue;
      }
      ImBuf *orig = shape_tile_backup_init(target.image, target.iuser, tile_number);
      if (orig) {
        backups_.append(TileBackup{target_index, tile_number, orig, rcti{0, 0, 0, 0}});
      }
    }
  }

  /* Restore what the previous preview painted (plus what this one will paint), then composite the
   * new regions — no full-tile restore per mouse move. */
  for (TileBackup &backup : backups_) {
    const ShapeTarget &target = targets_[backup.target_index];
    const rcti new_dirty = composite_tile_region_get(
        target.image, target.iuser, backup.tile_number, shapes, tile_, style);
    rcti restore_rect = backup.dirty;
    if (BLI_rcti_is_empty(&restore_rect)) {
      restore_rect = new_dirty;
    }
    else {
      shape_region_union(restore_rect, new_dirty);
    }
    if (!BLI_rcti_is_empty(&restore_rect)) {
      shape_tile_backup_restore_region(
          target.image, target.iuser, backup.tile_number, *backup.orig, restore_rect);
    }
    backup.dirty = new_dirty;
  }

  if (backups_.is_empty()) {
    return false;
  }
  shape_composite_into(targets_, shapes, tile_, style, false, "", nullptr);

  /* The 3D Viewport's live PBR preview only follows a shading-relevant image tag (the 2D brush
   * sends one per redraw), but tagging per mouse move re-evaluates the shading constantly, so the
   * preview throttles the tag instead of dropping it. The commit tags fully. */
  const double now = BLI_time_now_seconds();
  if (now - last_shading_tag_ >= SHAPE_PREVIEW_SHADING_TAG_INTERVAL) {
    last_shading_tag_ = now;
    for (const ShapeTarget &target : targets_) {
      if (target.channel >= 0 && target.image) {
        DEG_id_tag_update(&target.image->id, ID_RECALC_SHADING | ID_RECALC_PARAMETERS);
      }
    }
  }
  return true;
}

void ImageTilesBackend::restore_all()
{
  for (const TileBackup &backup : backups_) {
    const ShapeTarget &target = targets_[backup.target_index];
    /* A target freed by an undo cannot be restored; restoring the others is all that is left. */
    if (!this->target_alive(target)) {
      continue;
    }
    shape_tile_backup_restore(target.image, target.iuser, backup.tile_number, *backup.orig);
  }
}

bool ImageTilesBackend::commit(bContext * /*C*/,
                               ReportList * /*reports*/,
                               const Span<PaintShape> shapes,
                               const ShapeStyle &style,
                               const char *undo_name)
{
  /* The preview already wrote into these tiles; restore them first so the commit's undo step
   * captures the pre-session pixels (and so a bail-out leaves no preview trace), then bake once. */
  restore_all();
  if (targets_.is_empty() || shapes.is_empty()) {
    return false;
  }
  /* Do not bake into a target an undo / material edit has already replaced. The commit
   * failing here is what makes the Vector session cancel with a report instead of writing. */
  if (!this->targets_alive()) {
    return false;
  }
  return shape_composite_into(targets_, shapes, tile_, style, true, undo_name, ob_);
}

void ImageTilesBackend::cancel()
{
  restore_all();
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
