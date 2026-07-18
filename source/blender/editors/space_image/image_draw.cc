/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 */

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "DNA_mask_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view2d_types.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "IMB_cache.hh"
#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_enums.h"
#include "IMB_imbuf_types.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_paint.hh"

#include "BIF_glutil.hh"

#include "GPU_batch.hh"
#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"

#include "BLF_api.hh"

#include "ED_gpencil_legacy.hh"
#include "ED_image.hh"
#include "ED_mask.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "ED_util.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "RE_engine.h"
#include "RE_pipeline.h"

#include "image_intern.hh"
#include "image_runtime.hh"

namespace blender {

static void draw_render_info(
    const bContext *C, Scene *scene, Image *ima, ARegion *region, float zoomx, float zoomy)
{
  Render *re = RE_GetSceneRender(scene);
  Scene *stats_scene = ED_render_job_get_scene(C);
  if (stats_scene == nullptr) {
    stats_scene = scene;
  }

  RenderResult *rr = BKE_image_acquire_renderresult(stats_scene, ima);

  if (rr && rr->text) {
    float fill_color[4] = {0.0f, 0.0f, 0.0f, 0.25f};
    ED_region_info_draw(region, rr->text, fill_color, true);
  }

  BKE_image_release_renderresult(stats_scene, ima, rr);

  if (re) {
    int total_tiles;
    const rcti *tiles = RE_engine_get_current_tiles(re, &total_tiles);

    if (total_tiles) {
      /* find window pixel coordinates of origin */
      int x, y;
      ui::view2d_view_to_region(&region->v2d, 0.0f, 0.0f, &x, &y);

      GPU_matrix_push();
      GPU_matrix_translate_2f(x, y);
      GPU_matrix_scale_2f(zoomx, zoomy);

      uint pos = GPU_vertformat_attr_add(
          immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
      immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
      immUniformThemeColor(TH_FACE_SELECT);

      GPU_line_width(1.0f);

      const rcti *tile = tiles;
      for (int i = 0; i < total_tiles; i++, tile++) {
        immDrawBorderCorners(pos, tile, zoomx, zoomy);
      }

      immUnbindProgram();

      GPU_matrix_pop();
    }
  }
}

void ED_image_draw_info(Scene *scene,
                        ARegion *region,
                        bool color_manage,
                        bool use_default_view,
                        int channels,
                        int x,
                        int y,
                        const uchar cp[4],
                        const float fp[4],
                        const float linearcol[4])
{
  rcti color_rect;
  char str[256];
  int dx = 6;
  /* local coordinate visible rect inside region, to accommodate overlapping ui */
  const rcti *rect = ED_region_visible_rect(region);
  const int ymin = rect->ymin;
  const int dy = ymin + 0.3f * UI_UNIT_Y;

/* text colors */
/* XXX colored text not allowed in Blender UI */
#if 0
  uchar red[3] = {255, 50, 50};
  uchar green[3] = {0, 255, 0};
  uchar blue[3] = {100, 100, 255};
#else
  const uchar red[3] = {255, 255, 255};
  const uchar green[3] = {255, 255, 255};
  const uchar blue[3] = {255, 255, 255};
#endif
  float hue = 0, sat = 0, val = 0, lum = 0, u = 0, v = 0;
  float col[4], finalcol[4];

  GPU_blend(GPU_BLEND_ALPHA);

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  /* noisy, high contrast make impossible to read if lower alpha is used. */
  immUniformColor4ub(0, 0, 0, 190);
  immRectf(pos, 0, ymin, BLI_rcti_size_x(&region->winrct) + 1, ymin + UI_UNIT_Y);

  immUnbindProgram();

  GPU_blend(GPU_BLEND_NONE);

  BLF_size(blf_mono_font, 11.0f * UI_SCALE_FAC);

  BLF_color3ub(blf_mono_font, 255, 255, 255);
  SNPRINTF_UTF8(str, "X:%-4d  Y:%-4d |", x, y);
  BLF_position(blf_mono_font, dx, dy, 0);
  BLF_draw(blf_mono_font, str, sizeof(str));
  dx += BLF_width(blf_mono_font, str, sizeof(str));

  if (channels == 1 && (cp != nullptr || fp != nullptr)) {
    if (fp != nullptr) {
      SNPRINTF_UTF8(str, " Val:%-.3f |", fp[0]);
    }
    else if (cp != nullptr) {
      SNPRINTF_UTF8(str, " Val:%-.3f |", cp[0] / 255.0f);
    }
    BLF_color3ub(blf_mono_font, 255, 255, 255);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));
  }

  if (channels >= 3) {
    BLF_color3ubv(blf_mono_font, red);
    if (fp) {
      SNPRINTF_UTF8(str, "  R:%-.5f", fp[0]);
    }
    else if (cp) {
      SNPRINTF_UTF8(str, "  R:%-3d", cp[0]);
    }
    else {
      STRNCPY_UTF8(str, "  R:-");
    }
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    BLF_color3ubv(blf_mono_font, green);
    if (fp) {
      SNPRINTF_UTF8(str, "  G:%-.5f", fp[1]);
    }
    else if (cp) {
      SNPRINTF_UTF8(str, "  G:%-3d", cp[1]);
    }
    else {
      STRNCPY_UTF8(str, "  G:-");
    }
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    BLF_color3ubv(blf_mono_font, blue);
    if (fp) {
      SNPRINTF_UTF8(str, "  B:%-.5f", fp[2]);
    }
    else if (cp) {
      SNPRINTF_UTF8(str, "  B:%-3d", cp[2]);
    }
    else {
      STRNCPY_UTF8(str, "  B:-");
    }
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    if (channels == 4) {
      BLF_color3ub(blf_mono_font, 255, 255, 255);
      if (fp) {
        SNPRINTF_UTF8(str, "  A:%-.4f", fp[3]);
      }
      else if (cp) {
        SNPRINTF_UTF8(str, "  A:%-3d", cp[3]);
      }
      else {
        STRNCPY_UTF8(str, "- ");
      }
      BLF_position(blf_mono_font, dx, dy, 0);
      BLF_draw(blf_mono_font, str, sizeof(str));
      dx += BLF_width(blf_mono_font, str, sizeof(str));
    }

    if (color_manage) {
      float rgba[4];

      copy_v3_v3(rgba, linearcol);
      if (channels == 3) {
        rgba[3] = 1.0f;
      }
      else {
        rgba[3] = linearcol[3];
      }

      IMB_colormanagement_pixel_to_display_space_v4(rgba,
                                                    rgba,
                                                    (use_default_view) ? nullptr :
                                                                         &scene->view_settings,
                                                    &scene->display_settings,
                                                    DISPLAY_SPACE_COLOR_INSPECTION);

      SNPRINTF_UTF8(str, "  |  Display  R:%-.4f  G:%-.4f  B:%-.4f", rgba[0], rgba[1], rgba[2]);
      BLF_position(blf_mono_font, dx, dy, 0);
      BLF_draw(blf_mono_font, str, sizeof(str));
      dx += BLF_width(blf_mono_font, str, sizeof(str));
    }
  }

  /* color rectangle */
  if (channels == 1) {
    if (fp) {
      col[0] = col[1] = col[2] = fp[0];
    }
    else if (cp) {
      col[0] = col[1] = col[2] = float(cp[0]) / 255.0f;
    }
    else {
      col[0] = col[1] = col[2] = 0.0f;
    }
    col[3] = 1.0f;
  }
  else if (channels == 3) {
    copy_v3_v3(col, linearcol);
    col[3] = 1.0f;
  }
  else if (channels == 4) {
    copy_v4_v4(col, linearcol);
  }
  else {
    BLI_assert(0);
    zero_v4(col);
  }

  if (color_manage) {
    IMB_colormanagement_pixel_to_display_space_v4(finalcol,
                                                  col,
                                                  (use_default_view) ? nullptr :
                                                                       &scene->view_settings,
                                                  &scene->display_settings);
  }
  else {
    copy_v4_v4(finalcol, col);
  }

  GPU_blend(GPU_BLEND_NONE);
  dx += 0.25f * UI_UNIT_X;

  BLI_rcti_init(&color_rect,
                dx,
                dx + (1.5f * UI_UNIT_X),
                ymin + 0.15f * UI_UNIT_Y,
                ymin + 0.85f * UI_UNIT_Y);

  /* BLF uses immediate mode too, so we must reset our vertex format */
  pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

  if (channels == 4) {
    rcti color_rect_half;
    int color_quater_x, color_quater_y;

    color_rect_half = color_rect;
    color_rect_half.xmax = BLI_rcti_cent_x(&color_rect);
    /* what color ??? */
    immRectf(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);

    color_rect_half = color_rect;
    color_rect_half.xmin = BLI_rcti_cent_x(&color_rect);

    color_quater_x = BLI_rcti_cent_x(&color_rect_half);
    color_quater_y = BLI_rcti_cent_y(&color_rect_half);

    immUniformColor3ub(UI_ALPHA_CHECKER_DARK, UI_ALPHA_CHECKER_DARK, UI_ALPHA_CHECKER_DARK);
    immRectf(pos,
             color_rect_half.xmin,
             color_rect_half.ymin,
             color_rect_half.xmax,
             color_rect_half.ymax);

    immUniformColor3ub(UI_ALPHA_CHECKER_LIGHT, UI_ALPHA_CHECKER_LIGHT, UI_ALPHA_CHECKER_LIGHT);
    immRectf(pos, color_quater_x, color_quater_y, color_rect_half.xmax, color_rect_half.ymax);
    immRectf(pos, color_rect_half.xmin, color_rect_half.ymin, color_quater_x, color_quater_y);

    if (fp != nullptr || cp != nullptr) {
      GPU_blend(GPU_BLEND_ALPHA);
      immUniformColor3fvAlpha(finalcol, fp ? fp[3] : (cp[3] / 255.0f));
      immRectf(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);
      GPU_blend(GPU_BLEND_NONE);
    }
  }
  else {
    immUniformColor3fv(finalcol);
    immRectf(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);
  }
  immUnbindProgram();

  /* draw outline */
  pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor3ub(128, 128, 128);
  imm_draw_box_wire_2d(pos, color_rect.xmin, color_rect.ymin, color_rect.xmax, color_rect.ymax);
  immUnbindProgram();

  dx += 1.75f * UI_UNIT_X;

  BLF_color3ub(blf_mono_font, 255, 255, 255);
  if (channels == 1) {
    if (fp) {
      rgb_to_hsv(fp[0], fp[0], fp[0], &hue, &sat, &val);
      rgb_to_yuv(fp[0], fp[0], fp[0], &lum, &u, &v, BLI_YUV_ITU_BT709);
    }
    else if (cp) {
      rgb_to_hsv(
          float(cp[0]) / 255.0f, float(cp[0]) / 255.0f, float(cp[0]) / 255.0f, &hue, &sat, &val);
      rgb_to_yuv(float(cp[0]) / 255.0f,
                 float(cp[0]) / 255.0f,
                 float(cp[0]) / 255.0f,
                 &lum,
                 &u,
                 &v,
                 BLI_YUV_ITU_BT709);
    }

    SNPRINTF_UTF8(str, "V:%-.4f", val);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "   L:%-.4f", lum);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
  }
  else if (channels >= 3) {
    rgb_to_hsv(finalcol[0], finalcol[1], finalcol[2], &hue, &sat, &val);
    rgb_to_yuv(finalcol[0], finalcol[1], finalcol[2], &lum, &u, &v, BLI_YUV_ITU_BT709);

    SNPRINTF_UTF8(str, "H:%-.4f", hue);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "  S:%-.4f", sat);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "  V:%-.4f", val);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
    dx += BLF_width(blf_mono_font, str, sizeof(str));

    SNPRINTF_UTF8(str, "   L:%-.4f", lum);
    BLF_position(blf_mono_font, dx, dy, 0);
    BLF_draw(blf_mono_font, str, sizeof(str));
  }
}
void draw_image_sample_line(SpaceImage *sima)
{
  if (sima->sample_line_hist.flag & HISTO_FLAG_SAMPLELINE) {
    Histogram *hist = &sima->sample_line_hist;

    GPUVertFormat *format = immVertexFormat();
    uint shdr_dashed_pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);

    immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);

    float viewport_size[4];
    GPU_viewport_size_get_f(viewport_size);
    immUniform2f(
        "viewport_size", viewport_size[2] / UI_SCALE_FAC, viewport_size[3] / UI_SCALE_FAC);

    immUniform1i("colors_len", 2); /* Advanced dashes. */
    immUniform4f("color", 1.0f, 1.0f, 1.0f, 1.0f);
    immUniform4f("color2", 0.0f, 0.0f, 0.0f, 1.0f);
    immUniform1f("dash_width", 2.0f);
    immUniform1f("udash_factor", 0.5f);

    immBegin(GPU_PRIM_LINES, 2);
    immVertex2fv(shdr_dashed_pos, hist->co[0]);
    immVertex2fv(shdr_dashed_pos, hist->co[1]);
    immEnd();

    immUnbindProgram();
  }
}

/* Length of one dash-plus-gap of the selection outline, in UI-scaled pixels. */
static constexpr float SELECTION_MASK_DASH_WIDTH = 8.0f;
/* Marching-ants speed, in whole dash periods per second. */
static constexpr double SELECTION_MASK_DASH_SPEED = 1.0;

/**
 * Append the outline of the selected pixels of \a mask to \a r_verts as #GPU_PRIM_LINES vertex
 * pairs, in UV space (the tile at \a uv_origin covers one unit square).
 *
 * The winding of each segment encodes which side of it is selected, which is what makes the
 * animated dashes appear to travel around the selection in a consistent direction.
 */
static void image_selection_outline_build_tile(const ImBuf *mask,
                                               const float2 &uv_origin,
                                               Vector<float3> &r_verts)
{
  const float *data = mask->float_buffer.data;
  if (!data) {
    return;
  }

  auto append_segment = [&](const float2 &a, const float2 &b, const bool reversed) {
    if (reversed) {
      r_verts.append(float3(b.x, b.y, 0.0f));
      r_verts.append(float3(a.x, a.y, 0.0f));
    }
    else {
      r_verts.append(float3(a.x, a.y, 0.0f));
      r_verts.append(float3(b.x, b.y, 0.0f));
    }
  };

  /* Horizontal edges - borders between selected and unselected rows. */
  for (int y = 0; y <= mask->y; y++) {
    int x_start = -1;
    bool current_bot_sel = false;
    for (int x = 0; x <= mask->x; x++) {
      bool is_edge = false;
      bool bot_sel = false;
      if (x < mask->x) {
        if (y < mask->y) {
          const bool top_sel = (y > 0) && (data[(y - 1) * mask->x + x] > 0.5f);
          bot_sel = (data[y * mask->x + x] > 0.5f);
          is_edge = (top_sel != bot_sel);
        }
        else {
          /* Top image boundary: edge exists if the topmost pixel is selected. */
          is_edge = (data[(y - 1) * mask->x + x] > 0.5f);
          bot_sel = false;
        }
      }

      if (is_edge) {
        if (x_start < 0) {
          x_start = x;
          current_bot_sel = bot_sel;
        }
        else if (current_bot_sel != bot_sel) {
          /* Edge type changed (Top vs Bottom), emit previous segment. */
          const float fy = uv_origin.y + (float(y) / float(mask->y));
          append_segment({uv_origin.x + (float(x_start) / float(mask->x)), fy},
                         {uv_origin.x + (float(x) / float(mask->x)), fy},
                         current_bot_sel);
          x_start = x;
          current_bot_sel = bot_sel;
        }
      }
      else if (x_start >= 0) {
        const float fy = uv_origin.y + (float(y) / float(mask->y));
        append_segment({uv_origin.x + (float(x_start) / float(mask->x)), fy},
                       {uv_origin.x + (float(x) / float(mask->x)), fy},
                       current_bot_sel);
        x_start = -1;
      }
    }
  }

  /* Vertical edges - borders between selected and unselected columns. */
  for (int x = 0; x <= mask->x; x++) {
    int y_start = -1;
    bool current_right_sel = false;
    for (int y = 0; y <= mask->y; y++) {
      bool is_edge = false;
      bool right_sel = false;
      if (y < mask->y) {
        if (x < mask->x) {
          const bool left_sel = (x > 0) && (data[y * mask->x + (x - 1)] > 0.5f);
          right_sel = (data[y * mask->x + x] > 0.5f);
          is_edge = (left_sel != right_sel);
        }
        else {
          /* Right image boundary: edge exists if the rightmost pixel is selected. */
          is_edge = (data[y * mask->x + (x - 1)] > 0.5f);
          right_sel = false;
        }
      }

      if (is_edge) {
        if (y_start < 0) {
          y_start = y;
          current_right_sel = right_sel;
        }
        else if (current_right_sel != right_sel) {
          /* Edge type changed (Left vs Right), emit previous segment. */
          const float fx = uv_origin.x + (float(x) / float(mask->x));
          append_segment({fx, uv_origin.y + (float(y_start) / float(mask->y))},
                         {fx, uv_origin.y + (float(y) / float(mask->y))},
                         !current_right_sel);
          y_start = y;
          current_right_sel = right_sel;
        }
      }
      else if (y_start >= 0) {
        const float fx = uv_origin.x + (float(x) / float(mask->x));
        append_segment({fx, uv_origin.y + (float(y_start) / float(mask->y))},
                       {fx, uv_origin.y + (float(y) / float(mask->y))},
                       !current_right_sel);
        y_start = -1;
      }
    }
  }
}

/**
 * Rebuild #SpaceImage_Runtime::selection_outline_batch from the image's selection masks.
 *
 * Extracting the outline is O(width * height) per tile, so this must only run when the masks
 * actually changed. A null batch is a valid result and means "nothing to draw"; the revision is
 * still recorded so an empty selection does not trigger a rescan on every redraw.
 */
static void image_selection_outline_batch_ensure(ed::image::SpaceImage_Runtime &runtime,
                                                 const Image *ima)
{
  const uint64_t revision = BKE_image_paint_selection_mask_revision_get(ima);
  if (runtime.selection_outline_image == ima && runtime.selection_outline_revision == revision) {
    /* Cache is current. A null batch is a valid cached result meaning "nothing to draw".
     * The initial state (null image) never matches, so the first draw always builds. */
    return;
  }

  GPU_BATCH_DISCARD_SAFE(runtime.selection_outline_batch);
  runtime.selection_outline_image = ima;
  runtime.selection_outline_revision = revision;

  Vector<float3> verts;
  for (const ImageTile *tile : ConstListBaseWrapper<ImageTile>(ima->tiles)) {
    /* Read-only: the const overload must be used so drawing does not advance the revision it is
     * caching against. */
    const ImBuf *mask = BKE_image_paint_selection_mask_lookup(ima, tile->tile_number);
    if (!mask) {
      continue;
    }
    const float2 uv_origin(float((tile->tile_number - 1001) % 10),
                           float((tile->tile_number - 1001) / 10));
    image_selection_outline_build_tile(mask, uv_origin, verts);
  }

  if (verts.is_empty()) {
    return;
  }

  GPUVertFormat format = {0};
  const uint pos = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, uint(verts.size()));
  for (const int64_t i : verts.index_range()) {
    GPU_vertbuf_attr_set(vbo, pos, uint(i), &verts[i]);
  }

  runtime.selection_outline_batch = GPU_batch_create_ex(
      GPU_PRIM_LINES, vbo, nullptr, GPU_BATCH_OWNS_VBO);
}

void draw_image_paint_selection_mask(const bContext *C, ARegion *region)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima->mode != SI_MODE_PAINT || !sima->runtime) {
    return;
  }

  const Image *ima = ED_space_image(sima);
  if (!ima) {
    return;
  }

  if (!BKE_image_paint_selection_mask_has_any(ima)) {
    /* The last mask was removed: release the cached geometry rather than keeping it alive until
     * the editor is closed. */
    GPU_BATCH_DISCARD_SAFE(sima->runtime->selection_outline_batch);
    sima->runtime->selection_outline_image = nullptr;
    return;
  }

  image_selection_outline_batch_ensure(*sima->runtime, ima);
  gpu::Batch *batch = sima->runtime->selection_outline_batch;
  if (!batch) {
    return;
  }

  /* The outline is cached in UV space, so it is drawn through the View2D matrix rather than in
   * region pixels. Both matrices are saved: this runs in the middle of #draw_image_main_helpers
   * and the drawers that follow rely on the ambient region-pixel space. */
  GPU_matrix_push_projection();
  GPU_matrix_push();
  ui::view2d_view_ortho(&region->v2d);

  GPU_batch_program_set_builtin(batch, GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR_ANIMATED);

  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  GPU_batch_uniform_2f(
      batch, "viewport_size", viewport_size[2] / UI_SCALE_FAC, viewport_size[3] / UI_SCALE_FAC);
  GPU_batch_uniform_1i(batch, "colors_len", 2);
  GPU_batch_uniform_4f(batch, "color", 0.4f, 0.4f, 0.4f, 1.0f);
  GPU_batch_uniform_4f(batch, "color2", 1.0f, 1.0f, 1.0f, 1.0f);
  GPU_batch_uniform_1f(batch, "dash_width", SELECTION_MASK_DASH_WIDTH);
  GPU_batch_uniform_1f(batch, "udash_factor", 0.5f);

  /* Animate the dashes so the selection region reads as active. The phase is the only thing that
   * changes per frame; the geometry above stays cached. Reduce the (large and growing) time value
   * modulo one period in double precision before narrowing, otherwise the animation grows visibly
   * jerky after Blender has been running for a while. */
  const float dash_phase = float(fmod(BLI_time_now_seconds() * SELECTION_MASK_DASH_SPEED, 1.0));
  GPU_batch_uniform_1f(batch, "dash_phase", dash_phase);

  GPU_line_width(1.0f);
  GPU_blend(GPU_BLEND_ALPHA);

  GPU_batch_draw(batch);

  GPU_blend(GPU_BLEND_NONE);
  GPU_matrix_pop();
  GPU_matrix_pop_projection();
}

void draw_image_main_helpers(const bContext *C, ARegion *region)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  Image *ima = ED_space_image(sima);

  const bool show_viewer = (ima && ima->source == IMA_SRC_VIEWER) != 0;
  const bool show_render = (show_viewer && ima->type == IMA_TYPE_R_RESULT) != 0;
  if (ima && show_render) {
    float zoomx, zoomy;
    ED_space_image_get_zoom(sima, region, &zoomx, &zoomy);
    draw_render_info(C, sima->iuser.scene, ima, region, zoomx, zoomy);
  }

  if (sima->mode == SI_MODE_UV) {
    const Scene *scene = CTX_data_scene(C);
    const ToolSettings *ts = scene->toolsettings;
    if (ts->uv_flag & UV_FLAG_CUSTOM_REGION) {
      draw_image_uv_custom_region(region, ts->uv_custom_region);
    }
  }

  /* Selection mask overlay for Texture Paint mode. */
  draw_image_paint_selection_mask(C, region);
}

bool ED_space_image_show_cache(const SpaceImage *sima)
{
  Image *image = ED_space_image(sima);
  Mask *mask = nullptr;
  if (sima->mode == SI_MODE_MASK) {
    mask = ED_space_image_get_mask(sima);
  }
  if (image == nullptr && mask == nullptr) {
    return false;
  }
  if (mask == nullptr) {
    return ELEM(image->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE);
  }
  return true;
}

bool ED_space_image_show_cache_and_mval_over(const SpaceImage *sima,
                                             ARegion *region,
                                             const int mval[2])
{
  const rcti *rect_visible = ED_region_visible_rect(region);
  if (mval[1] > rect_visible->ymin + (16 * UI_SCALE_FAC)) {
    return false;
  }
  return ED_space_image_show_cache(sima);
}

void draw_image_cache(const bContext *C, ARegion *region)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  Scene *scene = CTX_data_scene(C);
  Image *image = ED_space_image(sima);
  float x, cfra = scene->r.cfra, sfra = scene->r.sfra, efra = scene->r.efra,
           framelen = region->winx / (efra - sfra + 1);
  Mask *mask = nullptr;

  if (!ED_space_image_show_cache(sima)) {
    return;
  }

  if (sima->mode == SI_MODE_MASK) {
    mask = ED_space_image_get_mask(sima);
  }

  /* Local coordinate visible rect inside region, to accommodate overlapping ui. */
  const rcti *rect_visible = ED_region_visible_rect(region);
  const int region_bottom = rect_visible->ymin;

  GPU_blend(GPU_BLEND_ALPHA);

  /* Draw cache background. */
  ED_region_cache_draw_background(region);

  /* Draw cached segments. */
  if (image != nullptr && image->runtime->cache != nullptr &&
      ELEM(image->source, IMA_SRC_SEQUENCE, IMA_SRC_MOVIE))
  {
    int num_segments = 0;
    int *points = nullptr;

    std::scoped_lock lock(image->runtime->cache_mutex);
    IMB_cache_get_cache_segments(image->runtime->cache, IMB_PROXY_NONE, 0, &num_segments, &points);

    ED_region_cache_draw_cached_segments(
        region, num_segments, points, sfra + sima->iuser.offset, efra + sima->iuser.offset);
  }

  GPU_blend(GPU_BLEND_NONE);

  /* Draw current frame. */
  x = (cfra - sfra) / (efra - sfra + 1) * region->winx;

  uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformThemeColor(TH_CFRAME);
  immRectf(pos, x, region_bottom, x + ceilf(framelen), region_bottom + 8 * UI_SCALE_FAC);
  immUnbindProgram();

  ED_region_cache_draw_curfra_label(
      cfra, x + roundf(framelen / 2), region_bottom + 8.0f * UI_SCALE_FAC);

  if (mask != nullptr) {
    ED_mask_draw_frames(mask, region, cfra, sfra, efra);
  }
}

float ED_space_image_zoom_level(const View2D *v2d, const int grid_dimension)
{
  /* UV-space length per pixel */
  float xzoom = (v2d->cur.xmax - v2d->cur.xmin) / float(v2d->mask.xmax - v2d->mask.xmin);
  float yzoom = (v2d->cur.ymax - v2d->cur.ymin) / float(v2d->mask.ymax - v2d->mask.ymin);

  /* Zoom_factor for UV/Image editor is calculated based on:
   * - Default grid size on startup, which is 256x256 pixels
   * - How blend factor for grid lines is set up in the fragment shader `grid_frag.glsl`. */
  float zoom_factor;
  zoom_factor = (xzoom + yzoom) / 2.0f; /* Average for accuracy. */
  zoom_factor *= 256.0f / powf(grid_dimension, 2);
  return zoom_factor;
}

void ED_space_image_grid_steps(SpaceImage *sima,
                               float grid_steps_x[SI_GRID_STEPS_LEN],
                               float grid_steps_y[SI_GRID_STEPS_LEN],
                               const int grid_dimension)
{
  const eSpaceImage_GridShapeSource grid_shape_source = eSpaceImage_GridShapeSource(
      sima->grid_shape_source);
  for (int step = 0; step < SI_GRID_STEPS_LEN; step++) {
    switch (grid_shape_source) {
      case SI_GRID_SHAPE_DYNAMIC:
        grid_steps_x[step] = powf(grid_dimension, step - SI_GRID_STEPS_LEN);
        grid_steps_y[step] = powf(grid_dimension, step - SI_GRID_STEPS_LEN);
        break;
      case SI_GRID_SHAPE_FIXED:
        grid_steps_x[step] = 1.0f / sima->custom_grid_subdiv[0];
        grid_steps_y[step] = 1.0f / sima->custom_grid_subdiv[1];
        break;
      case SI_GRID_SHAPE_PIXEL: {
        int pixel_width = IMG_SIZE_FALLBACK;
        int pixel_height = IMG_SIZE_FALLBACK;
        ED_space_image_get_size(sima, &pixel_width, &pixel_height);
        BLI_assert(pixel_width > 0 && pixel_height > 0);
        grid_steps_x[step] = 1.0f / pixel_width;
        grid_steps_y[step] = 1.0f / pixel_height;
        break;
      }
      default:
        BLI_assert_unreachable();
    }
  }
}

float ED_space_image_increment_snap_value(const int grid_dimensions,
                                          const float grid_steps[SI_GRID_STEPS_LEN],
                                          const float zoom_factor)
{
  /* Small offset on each grid_steps[] so that snapping value doesn't change until grid lines are
   * significantly visible.
   * `Offset = 3/4 * (grid_steps[i] - (grid_steps[i] / grid_dimensions))`
   *
   * Refer `grid_frag.glsl` to find out when grid lines actually start appearing */

  for (int step = 0; step < SI_GRID_STEPS_LEN; step++) {
    float offset = (3.0f / 4.0f) * (grid_steps[step] - (grid_steps[step] / grid_dimensions));

    if ((grid_steps[step] - offset) > zoom_factor) {
      return grid_steps[step];
    }
  }

  /* Fallback */
  return grid_steps[0];
}

void draw_image_uv_custom_region(const ARegion *region, const rctf &custom_region)
{
  const uint shdr_pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);

  GPU_line_width(1.0f);

  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);

  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2] / UI_SCALE_FAC, viewport_size[3] / UI_SCALE_FAC);

  immUniform1i("colors_len", 0); /* "simple" mode */
  immUniform4f("color", 1.0f, 0.25f, 0.25f, 1.0f);
  immUniform1f("dash_width", 6.0f);
  immUniform1f("udash_factor", 0.5f);
  rcti region_rect;

  ui::view2d_view_to_region_rcti(&region->v2d, &custom_region, &region_rect);

  imm_draw_box_wire_2d(
      shdr_pos, region_rect.xmin, region_rect.ymin, region_rect.xmax, region_rect.ymax);

  immUnbindProgram();
}

}  // namespace blender
