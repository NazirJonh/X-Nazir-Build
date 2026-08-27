/* SPDX-FileCopyrightText: 2009 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */
#include "paint_cursor.hh"

#include "BLI_vector.hh"
#include <algorithm>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_axis_angle.hh"
#include "BLI_math_color.h"
#include "BLI_math_rotation.h"
#include "BLI_offset_indices.hh"
#include "BLI_rect.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_curve_types.h"
#include "DNA_curves_types.h"
#include "DNA_layer_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_workspace_types.h"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curve_legacy_convert.hh"
#include "BKE_curves.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "NOD_texture.h"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "wm_cursors.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "ED_image.hh"
#include "ED_paint_curve_draw.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "PRF_profile.hh"

#include "UI_resources.hh"

#include "paint_curve_intern.hh"
#include "paint_intern.hh"

namespace blender {

/* TODOs:
 *
 * Some of the cursor drawing code is doing non-draw stuff
 * (e.g. updating the brush rake angle). This should be cleaned up
 * still.
 *
 * There is also some ugliness with sculpt-specific code.
 */

struct TexSnapshot {
  gpu::Texture *overlay_texture;
  int winx;
  int winy;
  int old_size;
  float old_zoom;
  float old_radius;
  bool old_col;
  /** Identity of the cached overlay. Do not store an #MTex pointer: Material Paint previews
   * pass a stack #MTex rebuilt each cursor draw (#PaintCursorContext.material_preview_mtex_storage),
   * so pointer equality would miss every frame and resample the source. */
  const Tex *old_tex;
  /** #MTex.brush_map_mode at the last build. The buffer layout differs per map mode (View
   * normalizes to the brush radius, Tiled/Stencil fill the whole 512x512 buffer differently), so
   * changing a channel's own Mapping dropdown must invalidate the cache even though #old_tex
   * stays the same. */
  int old_map_mode;
  float old_rot;
  /** Falloff curve baked into color overlays (View). Must rebuild when the preset or curve
   * mapping changes. */
  int old_curve_preset;
  /** Alpha stroke-mask source (#use_alpha_stroke_mask). Null when the overlay is not masked. */
  const Tex *old_alpha_tex;
};

struct CursorSnapshot {
  gpu::Texture *overlay_texture;
  int size;
  int zoom;
  int curve_preset;
};

static TexSnapshot primary_snap = {nullptr};
static TexSnapshot secondary_snap = {nullptr};
static CursorSnapshot cursor_snap = {nullptr};

void paint_cursor_delete_textures()
{
  if (primary_snap.overlay_texture) {
    GPU_texture_free(primary_snap.overlay_texture);
  }
  if (secondary_snap.overlay_texture) {
    GPU_texture_free(secondary_snap.overlay_texture);
  }
  if (cursor_snap.overlay_texture) {
    GPU_texture_free(cursor_snap.overlay_texture);
  }

  memset(&primary_snap, 0, sizeof(TexSnapshot));
  memset(&secondary_snap, 0, sizeof(TexSnapshot));
  memset(&cursor_snap, 0, sizeof(CursorSnapshot));

  BKE_paint_invalidate_overlay_all();
}

void ED_paint_cursor_free_textures()
{
  paint_cursor_delete_textures();
}

namespace ed::sculpt_paint {

static bool paint_overlay_alpha_mask_mtex(const Paint *paint,
                                          const Brush *br,
                                          const ViewContext *vc,
                                          MTex &r_storage)
{
  if (paint == nullptr || br == nullptr || br->material_paint == nullptr || vc == nullptr ||
      vc->scene == nullptr || vc->scene->toolsettings == nullptr)
  {
    return false;
  }
  const PaintModeSettings &mode_settings = vc->scene->toolsettings->paint_mode;
  const BrushMaterialPaint &brush_paint = *br->material_paint;
  /* The channel set belongs to the paint mode the cursor is drawn for, not to Sculpt Mode: Texture
   * Paint and Sculpt keep independent visibility. */
  if (!BKE_paint_material_channel_masks_stroke(
          brush_paint, mode_settings, paint->visible_material_channels))
  {
    return false;
  }
  const BrushMaterialPaintChannel &alpha_channel =
      brush_paint.channels[PAINT_MATERIAL_CHANNEL_ALPHA];
  if (!BKE_paint_material_channel_has_source(alpha_channel)) {
    return false;
  }
  BKE_paint_material_channel_effective_mtex(brush_paint, alpha_channel, r_storage);
  if (ELEM(r_storage.brush_map_mode, MTEX_MAP_MODE_AREA, MTEX_MAP_MODE_3D)) {
    r_storage.brush_map_mode = MTEX_MAP_MODE_VIEW;
  }
  return r_storage.tex != nullptr;
}

static int same_tex_snap(TexSnapshot *snap,
                         const MTex *mtex,
                         ViewContext *vc,
                         bool col,
                         float zoom,
                         float radius,
                         int curve_preset,
                         const Tex *alpha_tex)
{
  return (/* make brush smaller shouldn't cause a resample */
          //(mtex->brush_map_mode != MTEX_MAP_MODE_VIEW ||
          //(BKE_brush_size_get(vc->scene, brush) <= snap->BKE_brush_size_get)) &&

          (mtex->brush_map_mode != MTEX_MAP_MODE_TILED ||
           (vc->region->winx == snap->winx && vc->region->winy == snap->winy &&
            radius == snap->old_radius)) &&
          (mtex->brush_map_mode == MTEX_MAP_MODE_STENCIL || snap->old_zoom == zoom) &&
          snap->old_col == col && snap->old_tex == mtex->tex &&
          snap->old_map_mode == mtex->brush_map_mode && snap->old_rot == mtex->rot &&
          (!col || snap->old_curve_preset == curve_preset) && snap->old_alpha_tex == alpha_tex);
}

static void make_tex_snap(TexSnapshot *snap,
                          ViewContext *vc,
                          float zoom,
                          float radius,
                          const MTex *mtex,
                          int curve_preset,
                          const Tex *alpha_tex)
{
  snap->old_zoom = zoom;
  snap->old_radius = radius;
  snap->winx = vc->region->winx;
  snap->winy = vc->region->winy;
  snap->old_tex = mtex->tex;
  snap->old_map_mode = mtex->brush_map_mode;
  snap->old_rot = mtex->rot;
  snap->old_curve_preset = curve_preset;
  snap->old_alpha_tex = alpha_tex;
}

struct LoadTexData {
  Brush *br;
  ViewContext *vc;

  const MTex *mtex;
  uchar *buffer;
  bool col;

  ImagePool *pool;
  int size;
  float rotation;
  float radius;
  const MTex *alpha_mtex;
};

static void load_tex_task_cb_ex(void *__restrict userdata,
                                const int j,
                                const TaskParallelTLS *__restrict tls)
{
  LoadTexData *data = static_cast<LoadTexData *>(userdata);
  Brush *br = data->br;
  ViewContext *vc = data->vc;

  const MTex *mtex = data->mtex;
  uchar *buffer = data->buffer;
  const bool col = data->col;

  ImagePool *pool = data->pool;
  const int size = data->size;
  const float rotation = data->rotation;
  const float radius = data->radius;

  bool convert_to_linear = false;
  const ColorSpace *colorspace = nullptr;

  const int thread_id = BLI_task_parallel_thread_id(tls);

  if (mtex->tex && mtex->tex->type == TEX_IMAGE && mtex->tex->ima) {
    ImBuf *tex_ibuf = BKE_image_pool_acquire_ibuf(mtex->tex->ima, &mtex->tex->iuser, pool);
    /* For consistency, sampling always returns color in linear space. */
    if (tex_ibuf && tex_ibuf->float_data() == nullptr) {
      convert_to_linear = true;
      colorspace = tex_ibuf->byte_buffer.colorspace;
    }
    BKE_image_pool_release_ibuf(mtex->tex->ima, tex_ibuf, pool);
  }

  for (int i = 0; i < size; i++) {
    /* Largely duplicated from tex_strength. */

    int index = j * size + i;

    float x = float(i) / size;
    float y = float(j) / size;
    float len;

    if (mtex->brush_map_mode == MTEX_MAP_MODE_TILED) {
      x *= vc->region->winx / radius;
      y *= vc->region->winy / radius;
    }
    else {
      x = (x - 0.5f) * 2.0f;
      y = (y - 0.5f) * 2.0f;
    }

    len = sqrtf(x * x + y * y);

    if (ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_TILED, MTEX_MAP_MODE_STENCIL) || len <= 1.0f) {
      /* It is probably worth optimizing for those cases where the texture is not rotated by
       * skipping the calls to atan2, sqrtf, sin, and cos. */
      if (mtex->tex && (rotation > 0.001f || rotation < -0.001f)) {
        const float angle = atan2f(y, x) + rotation;

        x = len * cosf(angle);
        y = len * sinf(angle);
      }

      float avg;
      float rgba[4];
      paint_get_tex_pixel(mtex, x, y, pool, thread_id, &avg, rgba);

      if (col) {
        const bool is_data = colorspace != nullptr &&
                             IMB_colormanagement_space_is_data(colorspace);
        if (convert_to_linear && !is_data) {
          IMB_colormanagement_colorspace_to_scene_linear_v3(rgba, colorspace);
        }
        /* Data/"Non-Color" byte values are already the stored encoding. Encoding them as sRGB
         * a second time makes the overlay paler than the source image. */
        if (!is_data) {
          linearrgb_to_srgb_v3_v3(rgba, rgba);
        }

        clamp_v4(rgba, 0.0f, 1.0f);

        /* View mapping used to hard-clip at the brush radius. Bake the distance falloff into
         * premultiplied alpha so the overlay matches F-key preview (soft contour, not a disc).
         * Tiled/Stencil cover the region/stencil rect, not the dab, so they stay unmasked. */
        if (!ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_TILED, MTEX_MAP_MODE_STENCIL)) {
          const float falloff = BKE_brush_curve_strength_clamped(br, len, 1.0f);
          rgba[0] *= falloff;
          rgba[1] *= falloff;
          rgba[2] *= falloff;
          rgba[3] *= falloff;
        }

        if (data->alpha_mtex != nullptr && data->alpha_mtex->tex != nullptr) {
          float mask_intensity;
          float mask_rgba[4];
          paint_get_tex_pixel(
              data->alpha_mtex, x, y, pool, thread_id, &mask_intensity, mask_rgba);
          CLAMP(mask_intensity, 0.0f, 1.0f);
          rgba[0] *= mask_intensity;
          rgba[1] *= mask_intensity;
          rgba[2] *= mask_intensity;
          rgba[3] *= mask_intensity;
        }

        buffer[index * 4] = rgba[0] * 255;
        buffer[index * 4 + 1] = rgba[1] * 255;
        buffer[index * 4 + 2] = rgba[2] * 255;
        buffer[index * 4 + 3] = rgba[3] * 255;
      }
      else {
        avg += br->texture_sample_bias;

        /* Clamp to avoid precision overflow. */
        CLAMP(avg, 0.0f, 1.0f);
        buffer[index] = 255 - uchar(255 * avg);
      }
    }
    else {
      if (col) {
        buffer[index * 4] = 0;
        buffer[index * 4 + 1] = 0;
        buffer[index * 4 + 2] = 0;
        buffer[index * 4 + 3] = 0;
      }
      else {
        buffer[index] = 0;
      }
    }
  }
}

static int load_tex(Paint *paint,
                    Brush *br,
                    ViewContext *vc,
                    float zoom,
                    bool col,
                    bool primary,
                    const MTex *mtex_override = nullptr)
{
  bool init;
  TexSnapshot *target;

  const MTex *mtex = mtex_override ? mtex_override : (primary) ? &br->mtex : &br->mask_mtex;
  const float radius = BKE_brush_radius_get(paint, br) * zoom;
  ePaintOverlayControlFlags overlay_flags = BKE_paint_get_overlay_flags();
  uchar *buffer = nullptr;

  int size;
  bool refresh;
  ePaintOverlayControlFlags invalid =
      ((primary) ? (overlay_flags & PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY) :
                   (overlay_flags & PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY));
  target = (primary) ? &primary_snap : &secondary_snap;

  const int curve_preset = br->curve_distance_falloff_preset;
  MTex alpha_mtex_storage = {};
  const MTex *alpha_mtex = nullptr;
  if (col && paint_overlay_alpha_mask_mtex(paint, br, vc, alpha_mtex_storage)) {
    alpha_mtex = &alpha_mtex_storage;
  }
  refresh = !target->overlay_texture || (invalid != 0) ||
            !same_tex_snap(target,
                           mtex,
                           vc,
                           col,
                           zoom,
                           radius,
                           curve_preset,
                           alpha_mtex != nullptr ? alpha_mtex->tex : nullptr) ||
            (col && (overlay_flags & PAINT_OVERLAY_INVALID_CURVE));

  init = (target->overlay_texture != nullptr);

  if (refresh) {
    ImagePool *pool = nullptr;
    /* Stencil is rotated later. */
    const float rotation = (mtex->brush_map_mode != MTEX_MAP_MODE_STENCIL) ? -mtex->rot : 0.0f;
    make_tex_snap(target,
                  vc,
                  zoom,
                  radius,
                  mtex,
                  curve_preset,
                  alpha_mtex != nullptr ? alpha_mtex->tex : nullptr);

    if (col) {
      BKE_curvemapping_init(br->curve_distance_falloff);
    }

    if (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) {
      int s = BKE_brush_radius_get(paint, br);
      int r = 1;

      for (s >>= 1; s > 0; s >>= 1) {
        r++;
      }

      size = (1 << r);

      size = std::max(size, 256);
      size = std::max(size, target->old_size);
    }
    else {
      size = 512;
    }

    if (target->old_size != size || target->old_col != col) {
      if (target->overlay_texture) {
        GPU_texture_free(target->overlay_texture);
        target->overlay_texture = nullptr;
      }
      init = false;

      target->old_size = size;
      target->old_col = col;
    }
    if (col) {
      buffer = MEM_new_array_uninitialized<uchar>(size * size * 4, "load_tex");
    }
    else {
      buffer = MEM_new_array_uninitialized<uchar>(size * size, "load_tex");
    }

    pool = BKE_image_pool_new();

    if (mtex->tex && mtex->tex->nodetree) {
      /* Has internal flag to detect it only does it once. */
      ntreeTexBeginExecTree(mtex->tex->nodetree);
    }
    if (alpha_mtex != nullptr && alpha_mtex->tex != nullptr && alpha_mtex->tex->nodetree &&
        alpha_mtex->tex != mtex->tex)
    {
      ntreeTexBeginExecTree(alpha_mtex->tex->nodetree);
    }

    LoadTexData data{};
    data.br = br;
    data.vc = vc;
    data.mtex = mtex;
    data.buffer = buffer;
    data.col = col;
    data.pool = pool;
    data.size = size;
    data.rotation = rotation;
    data.radius = radius;
    data.alpha_mtex = alpha_mtex;

    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    BLI_task_parallel_range(0, size, &data, load_tex_task_cb_ex, &settings);

    if (mtex->tex && mtex->tex->nodetree) {
      ntreeTexEndExecTree(mtex->tex->nodetree->runtime->execdata);
    }
    if (alpha_mtex != nullptr && alpha_mtex->tex != nullptr && alpha_mtex->tex->nodetree &&
        alpha_mtex->tex != mtex->tex)
    {
      ntreeTexEndExecTree(alpha_mtex->tex->nodetree->runtime->execdata);
    }

    if (pool) {
      BKE_image_pool_free(pool);
    }

    if (!target->overlay_texture) {
      gpu::TextureFormat format = col ? gpu::TextureFormat::UNORM_8_8_8_8 :
                                        gpu::TextureFormat::UNORM_8;
      eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
      target->overlay_texture = GPU_texture_create_2d(
          "paint_cursor_overlay", size, size, 1, format, usage, nullptr);
      GPU_texture_update(target->overlay_texture, GPU_DATA_UBYTE, buffer);

      if (!col) {
        GPU_texture_swizzle_set(target->overlay_texture, "rrrr");
      }
    }

    if (init) {
      GPU_texture_update(target->overlay_texture, GPU_DATA_UBYTE, buffer);
    }

    if (buffer) {
      MEM_delete(buffer);
    }
  }
  else {
    size = target->old_size;
  }

  BKE_paint_reset_overlay_invalid(invalid);

  return 1;
}

static void load_tex_cursor_task_cb(void *__restrict userdata,
                                    const int j,
                                    const TaskParallelTLS *__restrict /*tls*/)
{
  LoadTexData *data = static_cast<LoadTexData *>(userdata);
  Brush *br = data->br;

  uchar *buffer = data->buffer;

  const int size = data->size;

  for (int i = 0; i < size; i++) {
    /* Largely duplicated from tex_strength. */

    const int index = j * size + i;
    const float x = ((float(i) / size) - 0.5f) * 2.0f;
    const float y = ((float(j) / size) - 0.5f) * 2.0f;
    const float len = sqrtf(x * x + y * y);

    if (len <= 1.0f) {

      /* Falloff curve. */
      float avg = BKE_brush_curve_strength_clamped(br, len, 1.0f);

      buffer[index] = uchar(255 * avg);
    }
    else {
      buffer[index] = 0;
    }
  }
}

static int load_tex_cursor(Paint *paint, Brush *br, float zoom)
{
  bool init;

  ePaintOverlayControlFlags overlay_flags = BKE_paint_get_overlay_flags();
  uchar *buffer = nullptr;

  int size;
  const bool refresh = !cursor_snap.overlay_texture ||
                       (overlay_flags & PAINT_OVERLAY_INVALID_CURVE) || cursor_snap.zoom != zoom ||
                       cursor_snap.curve_preset != br->curve_distance_falloff_preset;

  init = (cursor_snap.overlay_texture != nullptr);

  if (refresh) {
    int s, r;

    cursor_snap.zoom = zoom;

    s = BKE_brush_radius_get(paint, br);
    r = 1;

    for (s >>= 1; s > 0; s >>= 1) {
      r++;
    }

    size = (1 << r);

    size = std::max(size, 256);
    size = std::max(size, cursor_snap.size);

    if (cursor_snap.size != size) {
      if (cursor_snap.overlay_texture) {
        GPU_texture_free(cursor_snap.overlay_texture);
        cursor_snap.overlay_texture = nullptr;
      }

      init = false;

      cursor_snap.size = size;
    }
    buffer = MEM_new_array_uninitialized<uchar>(size * size, "load_tex");

    BKE_curvemapping_init(br->curve_distance_falloff);

    LoadTexData data{};
    data.br = br;
    data.buffer = buffer;
    data.size = size;

    TaskParallelSettings settings;
    BLI_parallel_range_settings_defaults(&settings);
    BLI_task_parallel_range(0, size, &data, load_tex_cursor_task_cb, &settings);

    if (!cursor_snap.overlay_texture) {
      eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT;
      cursor_snap.overlay_texture = GPU_texture_create_2d(
          "cursor_snap_overaly", size, size, 1, gpu::TextureFormat::UNORM_8, usage, nullptr);
      GPU_texture_update(cursor_snap.overlay_texture, GPU_DATA_UBYTE, buffer);

      GPU_texture_swizzle_set(cursor_snap.overlay_texture, "rrrr");
    }

    if (init) {
      GPU_texture_update(cursor_snap.overlay_texture, GPU_DATA_UBYTE, buffer);
    }

    if (buffer) {
      MEM_delete(buffer);
    }
  }
  else {
    size = cursor_snap.size;
  }

  cursor_snap.curve_preset = br->curve_distance_falloff_preset;
  BKE_paint_reset_overlay_invalid(PAINT_OVERLAY_INVALID_CURVE);

  return 1;
}

/* Draw an overlay that shows what effect the brush's texture will
 * have on brush strength. */
static bool paint_draw_tex_overlay(Paint *paint,
                                   Brush *brush,
                                   ViewContext *vc,
                                   int x,
                                   int y,
                                   float zoom,
                                   const PaintMode mode,
                                   bool col,
                                   bool primary,
                                   const MTex *mtex_override = nullptr)
{
  rctf quad;
  /* Check for overlay mode. */

  const MTex *mtex = mtex_override ? mtex_override : (primary) ? &brush->mtex : &brush->mask_mtex;
  /* A material paint channel preview must not be gated by the brush's own texture-overlay
   * toggle: the user needs to see it to position the pattern, the same way Stencil mode itself
   * already ignores this toggle below. */
  bool valid = mtex_override ? true :
               (primary)     ? (brush->overlay_flags & BRUSH_OVERLAY_PRIMARY) != 0 :
                               (brush->overlay_flags & BRUSH_OVERLAY_SECONDARY) != 0;
  int overlay_alpha = (primary) ? brush->texture_overlay_alpha : brush->mask_overlay_alpha;

  if (mode == PaintMode::Texture3D) {
    if (primary && brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_DRAW) {
      /* All non-draw tools don't use the primary texture (clone, smear, soften.. etc). */
      return false;
    }
  }

  if (!(mtex->tex) ||
      !((mtex->brush_map_mode == MTEX_MAP_MODE_STENCIL) ||
        (valid && ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_VIEW, MTEX_MAP_MODE_TILED))))
  {
    return false;
  }

  bke::PaintRuntime *paint_runtime = paint->runtime;
  if (load_tex(paint, brush, vc, zoom, col, primary, mtex_override)) {
    GPU_color_mask(true, true, true, true);
    GPU_depth_test(GPU_DEPTH_NONE);

    if (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) {
      GPU_matrix_push();

      float center[2] = {
          paint_runtime->draw_anchored ? paint_runtime->anchored_initial_mouse[0] : x,
          paint_runtime->draw_anchored ? paint_runtime->anchored_initial_mouse[1] : y,
      };

      /* Brush rotation. */
      GPU_matrix_translate_2fv(center);
      GPU_matrix_rotate_2d(
          RAD2DEGF(primary ? paint_runtime->brush_rotation : paint_runtime->brush_rotation_sec));
      GPU_matrix_translate_2f(-center[0], -center[1]);

      /* Scale based on tablet pressure. */
      if (primary && paint_runtime->stroke_active && BKE_brush_use_size_pressure(brush)) {
        const float scale = paint_runtime->size_pressure_value;
        GPU_matrix_translate_2fv(center);
        GPU_matrix_scale_2f(scale, scale);
        GPU_matrix_translate_2f(-center[0], -center[1]);
      }

      if (paint_runtime->draw_anchored) {
        quad.xmin = center[0] - paint_runtime->anchored_size;
        quad.ymin = center[1] - paint_runtime->anchored_size;
        quad.xmax = center[0] + paint_runtime->anchored_size;
        quad.ymax = center[1] + paint_runtime->anchored_size;
      }
      else {
        const int radius = BKE_brush_radius_get(paint, brush) * zoom;
        quad.xmin = center[0] - radius;
        quad.ymin = center[1] - radius;
        quad.xmax = center[0] + radius;
        quad.ymax = center[1] + radius;
      }
    }
    else if (mtex->brush_map_mode == MTEX_MAP_MODE_TILED) {
      quad.xmin = 0;
      quad.ymin = 0;
      quad.xmax = BLI_rcti_size_x(&vc->region->winrct);
      quad.ymax = BLI_rcti_size_y(&vc->region->winrct);
    }
    /* Stencil code goes here. */
    else {
      if (primary) {
        quad.xmin = -brush->stencil_dimension[0];
        quad.ymin = -brush->stencil_dimension[1];
        quad.xmax = brush->stencil_dimension[0];
        quad.ymax = brush->stencil_dimension[1];
      }
      else {
        quad.xmin = -brush->mask_stencil_dimension[0];
        quad.ymin = -brush->mask_stencil_dimension[1];
        quad.xmax = brush->mask_stencil_dimension[0];
        quad.ymax = brush->mask_stencil_dimension[1];
      }
      GPU_matrix_push();
      if (primary) {
        GPU_matrix_translate_2fv(brush->stencil_pos);
      }
      else {
        GPU_matrix_translate_2fv(brush->mask_stencil_pos);
      }
      GPU_matrix_rotate_2d(RAD2DEGF(mtex->rot));
    }

    /* Set quad color. Colored overlay does not get blending. */
    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    uint texCoord = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

    /* Premultiplied alpha blending. */
    GPU_blend(GPU_BLEND_ALPHA_PREMULT);

    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);

    float final_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!col) {
      copy_v3_v3(final_color, U.sculpt_paint_overlay_col);
    }
    mul_v4_fl(final_color, overlay_alpha * 0.01f);
    immUniformColor4fv(final_color);

    gpu::Texture *texture = (primary) ? primary_snap.overlay_texture :
                                        secondary_snap.overlay_texture;

    GPUSamplerExtendMode extend_mode = (mtex->brush_map_mode == MTEX_MAP_MODE_VIEW) ?
                                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER :
                                           GPU_SAMPLER_EXTEND_MODE_REPEAT;
    immBindTextureSampler(
        "image", texture, {GPU_SAMPLER_FILTERING_LINEAR, extend_mode, extend_mode});

    /* Draw textured quad. */
    immBegin(GPU_PRIM_TRI_FAN, 4);
    immAttr2f(texCoord, 0.0f, 0.0f);
    immVertex2f(pos, quad.xmin, quad.ymin);
    immAttr2f(texCoord, 1.0f, 0.0f);
    immVertex2f(pos, quad.xmax, quad.ymin);
    immAttr2f(texCoord, 1.0f, 1.0f);
    immVertex2f(pos, quad.xmax, quad.ymax);
    immAttr2f(texCoord, 0.0f, 1.0f);
    immVertex2f(pos, quad.xmin, quad.ymax);
    immEnd();

    immUnbindProgram();

    GPU_texture_unbind(texture);

    if (ELEM(mtex->brush_map_mode, MTEX_MAP_MODE_STENCIL, MTEX_MAP_MODE_VIEW)) {
      GPU_matrix_pop();
    }
  }
  return true;
}

/* Draw an overlay that shows what effect the brush's texture will
 * have on brush strength. */
static bool paint_draw_cursor_overlay(Paint *paint, Brush *brush, int x, int y, float zoom)
{
  rctf quad;
  /* Check for overlay mode. */

  if (!(brush->overlay_flags & BRUSH_OVERLAY_CURSOR)) {
    return false;
  }

  if (load_tex_cursor(paint, brush, zoom)) {
    bool do_pop = false;
    float center[2];

    GPU_color_mask(true, true, true, true);
    GPU_depth_test(GPU_DEPTH_NONE);

    bke::PaintRuntime *paint_runtime = paint->runtime;
    if (paint_runtime->draw_anchored) {
      copy_v2_v2(center, paint_runtime->anchored_initial_mouse);
      quad.xmin = paint_runtime->anchored_initial_mouse[0] - paint_runtime->anchored_size;
      quad.ymin = paint_runtime->anchored_initial_mouse[1] - paint_runtime->anchored_size;
      quad.xmax = paint_runtime->anchored_initial_mouse[0] + paint_runtime->anchored_size;
      quad.ymax = paint_runtime->anchored_initial_mouse[1] + paint_runtime->anchored_size;
    }
    else {
      const int radius = BKE_brush_radius_get(paint, brush) * zoom;
      center[0] = x;
      center[1] = y;

      quad.xmin = x - radius;
      quad.ymin = y - radius;
      quad.xmax = x + radius;
      quad.ymax = y + radius;
    }

    /* Scale based on tablet pressure. */
    if (paint_runtime->stroke_active && BKE_brush_use_size_pressure(brush)) {
      do_pop = true;
      GPU_matrix_push();
      GPU_matrix_translate_2fv(center);
      GPU_matrix_scale_1f(paint_runtime->size_pressure_value);
      GPU_matrix_translate_2f(-center[0], -center[1]);
    }

    GPUVertFormat *format = immVertexFormat();
    uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
    uint texCoord = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

    GPU_blend(GPU_BLEND_ALPHA_PREMULT);

    immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);

    float final_color[4] = {UNPACK3(U.sculpt_paint_overlay_col), 1.0f};
    mul_v4_fl(final_color, brush->cursor_overlay_alpha * 0.01f);
    immUniformColor4fv(final_color);

    /* Draw textured quad. */
    immBindTextureSampler("image",
                          cursor_snap.overlay_texture,
                          {GPU_SAMPLER_FILTERING_LINEAR,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                           GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});

    immBegin(GPU_PRIM_TRI_FAN, 4);
    immAttr2f(texCoord, 0.0f, 0.0f);
    immVertex2f(pos, quad.xmin, quad.ymin);
    immAttr2f(texCoord, 1.0f, 0.0f);
    immVertex2f(pos, quad.xmax, quad.ymin);
    immAttr2f(texCoord, 1.0f, 1.0f);
    immVertex2f(pos, quad.xmax, quad.ymax);
    immAttr2f(texCoord, 0.0f, 1.0f);
    immVertex2f(pos, quad.xmin, quad.ymax);
    immEnd();

    GPU_texture_unbind(cursor_snap.overlay_texture);

    immUnbindProgram();

    if (do_pop) {
      GPU_matrix_pop();
    }
  }
  return true;
}

static bool paint_draw_alpha_overlay(Paint *paint,
                                     Brush *brush,
                                     ViewContext *vc,
                                     int x,
                                     int y,
                                     float zoom,
                                     PaintMode mode,
                                     const MTex *material_preview_mtex = nullptr)
{
  /* Color means that primary brush texture is colored and secondary is used for alpha/mask
   * control. A material paint channel preview is always colored: it previews the actual pattern
   * the user is about to paint, not a strength falloff. */
  bool col = material_preview_mtex != nullptr ||
             ELEM(mode, PaintMode::Texture3D, PaintMode::Texture2D, PaintMode::Vertex);

  bool alpha_overlay_active = false;

  ePaintOverlayControlFlags flags = BKE_paint_get_overlay_flags();
  GPUBlend blend_state = GPU_blend_get();
  GPUDepthTest depth_test = GPU_depth_test_get();

  /* Translate to region. */
  GPU_matrix_push();
  GPU_matrix_translate_2f(vc->region->winrct.xmin, vc->region->winrct.ymin);
  x -= vc->region->winrct.xmin;
  y -= vc->region->winrct.ymin;

  /* Colored overlay should be drawn separately. */
  if (col) {
    if (!(flags & PAINT_OVERLAY_OVERRIDE_PRIMARY)) {
      alpha_overlay_active = paint_draw_tex_overlay(
          paint, brush, vc, x, y, zoom, mode, true, true, material_preview_mtex);
    }
    /* Material Paint has no secondary/mask channel equivalent to preview. */
    if (!(flags & PAINT_OVERLAY_OVERRIDE_SECONDARY) && !material_preview_mtex) {
      alpha_overlay_active = paint_draw_tex_overlay(
          paint, brush, vc, x, y, zoom, mode, false, false);
    }
    if (!(flags & PAINT_OVERLAY_OVERRIDE_CURSOR)) {
      alpha_overlay_active = paint_draw_cursor_overlay(paint, brush, x, y, zoom);
    }
  }
  else {
    if (!(flags & PAINT_OVERLAY_OVERRIDE_PRIMARY) && (mode != PaintMode::Weight)) {
      alpha_overlay_active = paint_draw_tex_overlay(
          paint, brush, vc, x, y, zoom, mode, false, true);
    }
    if (!(flags & PAINT_OVERLAY_OVERRIDE_CURSOR)) {
      alpha_overlay_active = paint_draw_cursor_overlay(paint, brush, x, y, zoom);
    }
  }

  GPU_matrix_pop();
  GPU_blend(blend_state);
  GPU_depth_test(depth_test);

  /* #load_tex may refresh on this flag, but only #load_tex_cursor used to clear it. If the
   * cursor-curve overlay is off, the bit stuck and the color overlay resampled every frame. */
  BKE_paint_reset_overlay_invalid(PAINT_OVERLAY_INVALID_CURVE);

  return alpha_overlay_active;
}

/* paint_draw_curve_cursor and its helpers (paintcurve_theme_handle_color,
 * draw_handle_endpoint, draw_control_point, should_show_radius_handle,
 * draw_radius_handle, draw_bezier_handle_lines) were removed.
 * Replaced by the Overlay engine PaintCurveCursor (overlay_paint_curve_cursor.hh). */

static bool paint_use_2d_cursor(PaintMode mode)
{
  switch (mode) {
    case PaintMode::Sculpt:
    case PaintMode::Vertex:
    case PaintMode::Weight:
      return false;
    case PaintMode::Texture3D:
    case PaintMode::Texture2D:
    case PaintMode::VertexGPencil:
    case PaintMode::SculptGPencil:
    case PaintMode::WeightGPencil:
    case PaintMode::SculptCurves:
    case PaintMode::GPencil:
      return true;
    case PaintMode::Invalid:
      BLI_assert_unreachable();
  }
  return true;
}

static bool paint_cursor_context_init(bContext *C,
                                      const int2 &xy,
                                      const float2 &tilt,
                                      PaintCursorContext &pcontext)
{
  PRF_scope(ProfileCategory::Editor);
  ARegion *region = CTX_wm_region(C);
  if (region && region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }

  pcontext.region = region;
  pcontext.wm = CTX_wm_manager(C);
  pcontext.win = CTX_wm_window(C);
  pcontext.screen = CTX_wm_screen(C);
  pcontext.depsgraph = CTX_data_depsgraph_pointer(C);
  pcontext.scene = CTX_data_scene(C);
  pcontext.object = CTX_data_active_object(C);
  pcontext.paint = BKE_paint_get_active_from_context(C);
  if (pcontext.paint == nullptr) {
    return false;
  }
  pcontext.ups = &pcontext.paint->unified_paint_settings;
  pcontext.brush = BKE_paint_brush(pcontext.paint);
  if (pcontext.brush == nullptr) {
    return false;
  }
  pcontext.mode = BKE_paintmode_get_active_from_context(C);
  if (pcontext.mode == PaintMode::Sculpt) {
    pcontext.sd = CTX_data_tool_settings(C)->sculpt;
  }

  /* Material Paint previews the channel texture the user is about to paint instead of the
   * brush's own #mtex (normally unset in this workflow). Sculpt Paint and Image Editor 2D share
   * the same per-channel sources, so both fill the overlay here. */
  const bool try_material_preview =
      pcontext.brush->material_paint != nullptr &&
      ((pcontext.mode == PaintMode::Sculpt &&
        pcontext.brush->sculpt_brush_type == SCULPT_BRUSH_TYPE_PAINT) ||
       pcontext.mode == PaintMode::Texture2D);
  if (try_material_preview) {
    const PaintModeSettings &paint_mode_settings = CTX_data_tool_settings(C)->paint_mode;
    /* Image Editor Paint always uses the 2D cursor; do not require the Sculpt canvas enum, so a
     * displayed map (or IMAGE canvas with PBR sources) still previews. Sculpt keeps the canvas
     * gate so Texture Paint 3D without a material canvas does not pick up a leftover preview. */
    const bool canvas_ok = pcontext.mode == PaintMode::Texture2D ||
                           ELEM(paint_mode_settings.canvas_source,
                                PAINT_CANVAS_SOURCE_MATERIAL_PAINT,
                                PAINT_CANVAS_SOURCE_MATERIAL);
    if (canvas_ok) {
      const bool has_preview = BKE_paint_material_preview_mtex_get(
          *pcontext.brush->material_paint,
          paint_mode_settings,
          pcontext.paint != nullptr ? pcontext.paint->visible_material_channels :
                                      PAINT_MATERIAL_CHANNELS_VISIBLE_ALL,
          pcontext.material_preview_mtex_storage);
      /* Image Editor has no Area Plane overlay: 2D sampling remaps AREA/3D to View, and the
       * overlay drawer only accepts View/Tiled/Stencil. Match that so the source shows on the
       * brush circle (including F / change size). In Sculpt, idle View is still skipped — it
       * only doubles the circle — unless the size is being dragged (#draw_anchored). */
      if (pcontext.mode == PaintMode::Texture2D &&
          ELEM(pcontext.material_preview_mtex_storage.brush_map_mode,
               MTEX_MAP_MODE_AREA,
               MTEX_MAP_MODE_3D))
      {
        pcontext.material_preview_mtex_storage.brush_map_mode = MTEX_MAP_MODE_VIEW;
      }
      /* Tiled covers the whole viewport, obscuring the stroke result while painting; hide it
       * for the duration of the stroke and let it reappear once the stroke ends. */
      const bool hide_for_active_tiled_stroke =
          pcontext.material_preview_mtex_storage.brush_map_mode == MTEX_MAP_MODE_TILED &&
          pcontext.paint->runtime->stroke_active;
      const bool hide_idle_view_in_sculpt =
          pcontext.mode != PaintMode::Texture2D &&
          pcontext.material_preview_mtex_storage.brush_map_mode == MTEX_MAP_MODE_VIEW &&
          !pcontext.paint->runtime->draw_anchored;
      if (has_preview && !hide_idle_view_in_sculpt && !hide_for_active_tiled_stroke) {
        pcontext.material_preview_mtex = &pcontext.material_preview_mtex_storage;
      }
    }
  }

  if (ELEM(pcontext.mode,
           PaintMode::Sculpt,
           PaintMode::Vertex,
           PaintMode::Weight,
           PaintMode::Texture3D))
  {
    pcontext.base = CTX_data_active_base(C);
  }

  pcontext.vc = ED_view3d_viewcontext_init(C, pcontext.depsgraph);

  /* Curve drawing is now handled by the PaintCurveCursor overlay. */
  if (paint_use_2d_cursor(pcontext.mode)) {
    pcontext.cursor_type = PaintCursorDrawingType::Cursor2D;
  }
  else {
    pcontext.cursor_type = PaintCursorDrawingType::Cursor3D;
  }

  pcontext.mval = xy;
  pcontext.translation = {float(xy[0]), float(xy[1])};
  pcontext.tilt = tilt;

  float zoomx, zoomy;
  get_imapaint_zoom(C, &zoomx, &zoomy);
  pcontext.zoomx = max_ff(zoomx, zoomy);
  pcontext.final_radius = (BKE_brush_radius_get(pcontext.paint, pcontext.brush) * zoomx);

  const bke::PaintRuntime &paint_runtime = *pcontext.paint->runtime;
  /* There is currently no way to check if the direction is inverted before starting the stroke,
   * so this does not reflect the state of the brush in the UI. */
  if (((!paint_runtime.draw_inverted) ^ ((pcontext.brush->flag & BRUSH_DIR_IN) == 0)) &&
      bke::brush::supports_secondary_cursor_color(*pcontext.brush))
  {
    pcontext.outline_col = float3(pcontext.brush->sub_col);
  }
  else {
    pcontext.outline_col = float3(pcontext.brush->add_col);
  }
  pcontext.outline_alpha = pcontext.brush->add_col[3];

  Object *active_object = pcontext.vc.obact;
  pcontext.ss = active_object ? active_object->runtime->sculpt_session : nullptr;

  if (pcontext.ss && pcontext.ss->draw_faded_cursor) {
    pcontext.outline_alpha = 0.3f;
    pcontext.outline_col = float3(0.8f);
  }

  const ScrArea *area = CTX_wm_area(C);
  pcontext.is_brush_active = paint_brush_tool_poll(area, region, pcontext.paint, pcontext.object);
  if (!pcontext.is_brush_active) {
    /* Use a default color for tools that are not brushes. */
    pcontext.outline_alpha = 0.8f;
    pcontext.outline_col = float3(0.8f);
  }

  pcontext.is_stroke_active = paint_runtime.stroke_active;

  return true;
}

static void paint_update_mouse_cursor(PaintCursorContext &pcontext)
{
  if (pcontext.win->grabcursor != 0 || pcontext.win->modalcursor != 0) {
    /* Don't set the cursor while it's grabbed, since this will show the cursor when interacting
     * with the UI (dragging a number button for example), see: #102792.
     * And don't overwrite a modal cursor, allowing modal operators to set a cursor temporarily. */
    return;
  }

  /* Don't set the cursor when a temporary popup is opened (e.g. a context menu, pie menu or
   * dialog), see: #137386. */
  if (!pcontext.screen->regionbase.is_empty() &&
      (BKE_screen_find_region_type(pcontext.screen, RGN_TYPE_TEMPORARY) != nullptr))
  {
    return;
  }

  if (ELEM(pcontext.mode, PaintMode::GPencil, PaintMode::VertexGPencil)) {
    WM_cursor_set(pcontext.win, WM_CURSOR_DOT);
  }
  else {
    /* Don't use paint cursor when overlapping with the size circle. */
    const int brush_size = BKE_brush_size_get(pcontext.paint, pcontext.brush);
    const bool small = brush_size < 28 && brush_size > 12;
    WM_cursor_set(pcontext.win, small ? WM_CURSOR_DOT : WM_CURSOR_PAINT);
  }
}

static void paint_draw_2D_view_brush_cursor_default(PaintCursorContext &pcontext)
{
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha);
  const bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;

  /* Draw brush outline. */
  if (paint_runtime->stroke_active && BKE_brush_use_size_pressure(pcontext.brush)) {
    imm_draw_circle_wire_2d(pcontext.pos,
                            pcontext.translation[0],
                            pcontext.translation[1],
                            pcontext.final_radius * paint_runtime->size_pressure_value,
                            40);
    /* Outer at half alpha. */
    immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha * 0.5f);
  }

  GPU_line_width(1.0f);
  imm_draw_circle_wire_2d(
      pcontext.pos, pcontext.translation[0], pcontext.translation[1], pcontext.final_radius, 40);
}

static void paint_draw_2D_view_brush_cursor(PaintCursorContext &pcontext)
{
  PRF_scope(ProfileCategory::Draw);
  switch (pcontext.mode) {
    case PaintMode::GPencil:
    case PaintMode::VertexGPencil:
      grease_pencil_cursor_draw(pcontext);
      break;
    default:
      paint_draw_2D_view_brush_cursor_default(pcontext);
  }
}

static void paint_draw_legacy_3D_view_brush_cursor(PaintCursorContext &pcontext)
{
  PRF_scope(ProfileCategory::Draw);
  GPU_line_width(1.0f);
  immUniformColor3fvAlpha(pcontext.outline_col, pcontext.outline_alpha);
  imm_draw_circle_wire_3d(
      pcontext.pos, pcontext.translation[0], pcontext.translation[1], pcontext.final_radius, 40);
}

static void paint_cursor_draw_3D_view_brush_cursor(PaintCursorContext &pcontext)
{
  BLI_assert(ELEM(pcontext.mode,
                  PaintMode::Sculpt,
                  PaintMode::Vertex,
                  PaintMode::Weight,
                  PaintMode::Texture3D));
  /* These paint tools are not using the SculptSession, so they need to use the default 2D brush
   * cursor in the 3D view. */
  if (pcontext.mode == PaintMode::Texture3D) {
    paint_draw_legacy_3D_view_brush_cursor(pcontext);
    return;
  }

  BLI_assert(pcontext.ss);

  mesh_cursor_update_and_init(pcontext);

  if (pcontext.is_stroke_active) {
    mesh_cursor_active_draw(pcontext);
  }
  else {
    const Brush &brush = *pcontext.brush;
    /* 2D falloff is better represented with the default 2D cursor,
     * there is no need to draw anything else. */
    if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      paint_draw_legacy_3D_view_brush_cursor(pcontext);
      return;
    }
    if (pcontext.alpha_overlay_drawn) {
      paint_draw_legacy_3D_view_brush_cursor(pcontext);
      return;
    }

    mesh_cursor_inactive_draw(pcontext);
  }
}

static bool paint_cursor_is_3d_view_navigating(const PaintCursorContext &pcontext)
{
  const ViewContext *vc = &pcontext.vc;
  return vc->rv3d && (vc->rv3d->rflag & RV3D_NAVIGATING);
}

static bool paint_cursor_is_brush_cursor_enabled(const PaintCursorContext &pcontext)
{
  if (pcontext.paint->flags & PAINT_SHOW_BRUSH) {
    if (ELEM(pcontext.mode, PaintMode::Texture2D, PaintMode::Texture3D) &&
        pcontext.brush->image_brush_type == IMAGE_PAINT_BRUSH_TYPE_FILL)
    {
      return false;
    }
    return true;
  }
  return false;
}

static void paint_cursor_update_rake_rotation(PaintCursorContext &pcontext)
{
  PRF_scope(ProfileCategory::Editor);
  /* Don't calculate rake angles while a stroke is active because the rake variables are global
   * and we may get interference with the stroke itself.
   * For line strokes, such interference is visible. */
  const bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;
  if (!paint_runtime->stroke_active) {
    paint_calculate_rake_rotation(
        *pcontext.paint, *pcontext.brush, pcontext.translation, pcontext.mode, true);
  }
}

static void paint_cursor_check_and_draw_alpha_overlays(PaintCursorContext &pcontext)
{
  PRF_scope(ProfileCategory::Draw);
  pcontext.alpha_overlay_drawn = pcontext.is_brush_active &&
                                 paint_draw_alpha_overlay(pcontext.paint,
                                                          pcontext.brush,
                                                          &pcontext.vc,
                                                          pcontext.mval.x,
                                                          pcontext.mval.y,
                                                          pcontext.zoomx,
                                                          pcontext.mode,
                                                          pcontext.material_preview_mtex);
}

static void paint_cursor_update_anchored_location(PaintCursorContext &pcontext)
{
  bke::PaintRuntime *paint_runtime = pcontext.paint->runtime;
  if (paint_runtime->draw_anchored) {
    pcontext.final_radius = paint_runtime->anchored_size;
    pcontext.translation = {
        paint_runtime->anchored_initial_mouse[0] + pcontext.region->winrct.xmin,
        paint_runtime->anchored_initial_mouse[1] + pcontext.region->winrct.ymin};
  }
}

static void paint_cursor_setup_2D_drawing(PaintCursorContext &pcontext)
{
  GPU_line_width(2.0f);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  pcontext.pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
}

static void paint_cursor_setup_3D_drawing(PaintCursorContext &pcontext)
{
  GPU_line_width(2.0f);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  pcontext.pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
}

static void paint_cursor_restore_drawing_state()
{
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
  GPU_line_smooth(false);
}

static void paint_draw_cursor(bContext *C, const int2 &xy, const float2 &tilt, void * /*unused*/)
{
  PRF_scope(ProfileCategory::Default);
  PaintCursorContext pcontext;
  if (!paint_cursor_context_init(C, xy, tilt, pcontext)) {
    return;
  }

  if (!paint_cursor_is_brush_cursor_enabled(pcontext)) {
    /* For Grease Pencil draw mode, we want to we only render a small mouse cursor (dot) if the
     * paint cursor is disabled so that the default mouse cursor doesn't get in the way of tablet
     * users. See #130089. But don't overwrite a modal cursor, allowing modal operators to set one
     * temporarily. */
    if (pcontext.mode == PaintMode::GPencil && pcontext.win->modalcursor == 0) {
      WM_cursor_set(pcontext.win, WM_CURSOR_DOT);
    }
    return;
  }

  /* Suppress the default brush cursor (size circle and texture/strength overlays) whenever the
   * paint-curve overlay engine is responsible for feedback -- Curve, Curve Patch, Roll, and the
   * standalone Curve Edit tool. Without this, switching from Curve to Curve Patch leaves both the
   * paint-curve handles and the brush overlays visible at once. */
  const ScrArea *area = CTX_wm_area(C);
  const bool is_space_v3d = area && area->spacetype == SPACE_VIEW3D;
  const bool is_space_image = area && area->spacetype == SPACE_IMAGE;
  const bToolRef *tref = WM_toolsystem_ref_from_context(C);
  if (ed::sculpt_paint::ED_paint_curve_overlay_is_relevant(
          pcontext.brush, tref ? tref->idname : nullptr, is_space_v3d, is_space_image))
  {
    return;
  }

  if (paint_cursor_is_3d_view_navigating(pcontext)) {
    /* Still draw stencil while navigating. */
    paint_cursor_check_and_draw_alpha_overlays(pcontext);
    return;
  }

  switch (pcontext.cursor_type) {
    case PaintCursorDrawingType::Cursor2D:
      paint_update_mouse_cursor(pcontext);

      paint_cursor_update_rake_rotation(pcontext);
      paint_cursor_check_and_draw_alpha_overlays(pcontext);
      paint_cursor_update_anchored_location(pcontext);

      paint_cursor_setup_2D_drawing(pcontext);
      paint_draw_2D_view_brush_cursor(pcontext);
      paint_cursor_restore_drawing_state();
      break;
    case PaintCursorDrawingType::Cursor3D:
      paint_update_mouse_cursor(pcontext);

      paint_cursor_update_rake_rotation(pcontext);
      paint_cursor_check_and_draw_alpha_overlays(pcontext);
      paint_cursor_update_anchored_location(pcontext);

      paint_cursor_setup_3D_drawing(pcontext);
      paint_cursor_draw_3D_view_brush_cursor(pcontext);
      paint_cursor_restore_drawing_state();
      break;
    default:
      BLI_assert_unreachable();
  }
}

}  // namespace ed::sculpt_paint

/* Public API */

namespace {

/** Poll for the paint-curve overlay redraw cursor: tags the viewport for redraw on mouse move. */
static bool paint_curve_overlay_redraw_poll(bContext *C)
{
  ed::sculpt_paint::ED_paint_curve_patch_modal_handlers_ensure(C);
  if (!ed::sculpt_paint::ED_paint_curve_overlay_wants_redraw(C)) {
    return false;
  }
  ed::sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(C);
  return true;
}

/** Empty draw callback — the actual drawing happens in the Overlay engine. */
static void paint_curve_overlay_redraw_draw(bContext * /*C*/,
                                            const int2 & /*xy*/,
                                            const float2 & /*tilt*/,
                                            void * /*customdata*/)
{
}

}  // anonymous namespace

void ED_paint_curve_overlay_redraw_register()
{
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  WM_paint_cursor_activate(SPACE_TYPE_ANY,
                           RGN_TYPE_ANY,
                           paint_curve_overlay_redraw_poll,
                           paint_curve_overlay_redraw_draw,
                           nullptr);
}

void ED_paint_cursor_start(Paint *paint, bool (*poll)(bContext *C))
{
  if (paint && paint->runtime && !paint->runtime->paint_cursor) {
    paint->runtime->paint_cursor = WM_paint_cursor_activate(
        SPACE_TYPE_ANY, RGN_TYPE_ANY, poll, ed::sculpt_paint::paint_draw_cursor, nullptr);
  }

  /* Register the overlay-redraw cursor (once, guarded internally). */
  ED_paint_curve_overlay_redraw_register();

  /* Invalidate the paint cursors. */
  BKE_paint_invalidate_overlay_all();
}

/* ED_paint_draw_curve_view3d_overlay removed: drawing is now handled
 * by overlay_paint_curve_cursor.hh via the Overlay draw engine. */

}  // namespace blender
