/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 */

#pragma once

/* Debug flag for canvas rotation. Set to 0 to disable debug output. */
#ifndef IMAGE_ROTATION_DEBUG
#  define IMAGE_ROTATION_DEBUG 0
#endif

#include "ED_image.hh"

#include "DNA_screen_types.h"

#include "BLI_math_base.h"
#include "BLI_math_matrix.h"

#include "cstdio"

#include "image_private.hh"
#include "image_shader_shared.hh"

/* Debug printf macro */
#if IMAGE_ROTATION_DEBUG
#  define ROT_DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#  define ROT_DEBUG_PRINT(...) ((void)0)
#endif

namespace blender::image_engine {

class SpaceImageAccessor : public AbstractSpaceAccessor {
  SpaceImage *sima;

 public:
  SpaceImageAccessor(SpaceImage *sima) : sima(sima) {}

  blender::Image *get_image(Main * /*bmain*/) override
  {
    return ED_space_image(sima);
  }

  ImageUser *get_image_user() override
  {
    return &sima->iuser;
  }

  ImBuf *acquire_image_buffer(blender::Image * /*image*/, void **lock) override
  {
    return ED_space_image_acquire_buffer(sima, lock, 0);
  }

  void release_buffer(blender::Image * /*image*/, ImBuf *image_buffer, void *lock) override
  {
    ED_space_image_release_buffer(sima, image_buffer, lock);
  }

  void get_shader_parameters(ShaderParameters &r_shader_parameters, ImBuf *image_buffer) override
  {
    const int sima_flag = sima->flag & ED_space_image_get_display_channel_mask(image_buffer);
    if ((sima_flag & SI_USE_ALPHA) != 0) {
      /* Show RGBA */
      r_shader_parameters.flags |= IMAGE_DRAW_FLAG_SHOW_ALPHA | IMAGE_DRAW_FLAG_APPLY_ALPHA;
    }
    else if ((sima_flag & SI_SHOW_ALPHA) != 0) {
      r_shader_parameters.flags |= IMAGE_DRAW_FLAG_SHUFFLING;
      r_shader_parameters.shuffle = float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    else if ((sima_flag & SI_SHOW_ZBUF) != 0) {
      r_shader_parameters.flags |= IMAGE_DRAW_FLAG_DEPTH | IMAGE_DRAW_FLAG_SHUFFLING;
      r_shader_parameters.shuffle = float4(1.0f, 0.0f, 0.0f, 0.0f);
    }
    else if ((sima_flag & SI_SHOW_R) != 0) {
      r_shader_parameters.flags |= IMAGE_DRAW_FLAG_SHUFFLING;
      if (IMB_alpha_affects_rgb(image_buffer)) {
        r_shader_parameters.flags |= IMAGE_DRAW_FLAG_APPLY_ALPHA;
      }
      r_shader_parameters.shuffle = float4(1.0f, 0.0f, 0.0f, 0.0f);
    }
    else if ((sima_flag & SI_SHOW_G) != 0) {
      r_shader_parameters.flags |= IMAGE_DRAW_FLAG_SHUFFLING;
      if (IMB_alpha_affects_rgb(image_buffer)) {
        r_shader_parameters.flags |= IMAGE_DRAW_FLAG_APPLY_ALPHA;
      }
      r_shader_parameters.shuffle = float4(0.0f, 1.0f, 0.0f, 0.0f);
    }
    else if ((sima_flag & SI_SHOW_B) != 0) {
      r_shader_parameters.flags |= IMAGE_DRAW_FLAG_SHUFFLING;
      if (IMB_alpha_affects_rgb(image_buffer)) {
        r_shader_parameters.flags |= IMAGE_DRAW_FLAG_APPLY_ALPHA;
      }
      r_shader_parameters.shuffle = float4(0.0f, 0.0f, 1.0f, 0.0f);
    }
    else /* RGB */ {
      if (IMB_alpha_affects_rgb(image_buffer)) {
        r_shader_parameters.flags |= IMAGE_DRAW_FLAG_APPLY_ALPHA;
      }
    }
  }

  bool use_tile_drawing() const override
  {
    return (sima->flag & SI_DRAW_TILE) != 0;
  }

  bool use_display_window() const override
  {
    return sima->mode == SI_MODE_VIEW;
  }

  void init_ss_to_texture_matrix(const ARegion *region,
                                 const float image_offset[2],
                                 const float image_resolution[2],
                                 float r_uv_to_texture[4][4]) const override
  {
    ROT_DEBUG_PRINT("\n=== init_ss_to_texture_matrix DEBUG ===\n");
    ROT_DEBUG_PRINT("sima->rotation = %.6f radians (%.2f degrees)\n", sima->rotation, sima->rotation * 180.0f / M_PI);
    ROT_DEBUG_PRINT("image_offset[0] = %.6f, image_offset[1] = %.6f\n", image_offset[0], image_offset[1]);
    ROT_DEBUG_PRINT("image_resolution[0] = %.6f, image_resolution[1] = %.6f\n", image_resolution[0], image_resolution[1]);
    ROT_DEBUG_PRINT("region->v2d.cur: xmin=%.2f, xmax=%.2f, ymin=%.2f, ymax=%.2f\n",
           region->v2d.cur.xmin, region->v2d.cur.xmax, region->v2d.cur.ymin, region->v2d.cur.ymax);

    unit_m4(r_uv_to_texture);
    float scale_x = 1.0 / BLI_rctf_size_x(&region->v2d.cur);
    float scale_y = 1.0 / BLI_rctf_size_y(&region->v2d.cur);

    ROT_DEBUG_PRINT("BLI_rctf_size_x = %.6f, BLI_rctf_size_y = %.6f\n", BLI_rctf_size_x(&region->v2d.cur), BLI_rctf_size_y(&region->v2d.cur));
    ROT_DEBUG_PRINT("scale_x = %.6f, scale_y = %.6f\n", scale_x, scale_y);

    float display_offset_x = scale_x * image_offset[0] / image_resolution[0];
    float display_offset_y = scale_y * image_offset[1] / image_resolution[1];

    ROT_DEBUG_PRINT("display_offset_x = %.6f, display_offset_y = %.6f\n", display_offset_x, display_offset_y);

    float translate_x = scale_x * -region->v2d.cur.xmin + display_offset_x;
    float translate_y = scale_y * -region->v2d.cur.ymin + display_offset_y;

    ROT_DEBUG_PRINT("translate_x = %.6f, translate_y = %.6f\n", translate_x, translate_y);

    if (sima->rotation != 0.0f) {
      ROT_DEBUG_PRINT("\n--- ROTATION BRANCH ---\n");
      const float cos_r = cosf(sima->rotation);
      const float sin_r = sinf(sima->rotation);
      const float winx = float(region->winx);
      const float winy = float(region->winy);
      const float aspect_x = winx / winy;
      const float aspect_y = winy / winx;

      ROT_DEBUG_PRINT("  cos_r = %.6f, sin_r = %.6f\n", cos_r, sin_r);
      ROT_DEBUG_PRINT("  winx = %.1f, winy = %.1f\n", winx, winy);
      ROT_DEBUG_PRINT("  aspect_x = %.6f (winx/winy)\n", aspect_x);
      ROT_DEBUG_PRINT("  aspect_y = %.6f (winy/winx)\n", aspect_y);

      const float pivot_x = sima->rotation_pivot[0];
      const float pivot_y = sima->rotation_pivot[1];

      ROT_DEBUG_PRINT("  pivot UV: [%.6f, %.6f]\n", pivot_x, pivot_y);

      const float pivot_ss_x = scale_x * pivot_x;
      const float pivot_ss_y = scale_y * pivot_y;

      ROT_DEBUG_PRINT("  pivot_ss: [%.6f, %.6f] (scale * pivot)\n", pivot_ss_x, pivot_ss_y);

      const float pivot_ss_rot_x = cos_r * pivot_ss_x - (aspect_y * sin_r) * pivot_ss_y;
      const float pivot_ss_rot_y = (aspect_x * sin_r) * pivot_ss_x + cos_r * pivot_ss_y;

      ROT_DEBUG_PRINT("  pivot_ss_rot: [%.6f, %.6f] (rotated with aspect)\n", pivot_ss_rot_x, pivot_ss_rot_y);
      ROT_DEBUG_PRINT("  pivot delta (ss - ss_rot): [%.6f, %.6f]\n",
             pivot_ss_x - pivot_ss_rot_x, pivot_ss_y - pivot_ss_rot_y);

      r_uv_to_texture[0][0] = cos_r * scale_x;
      r_uv_to_texture[0][1] = (aspect_x * sin_r) * scale_x;
      r_uv_to_texture[1][0] = -(aspect_y * sin_r) * scale_y;
      r_uv_to_texture[1][1] = cos_r * scale_y;

      ROT_DEBUG_PRINT("\n  Matrix components:\n");
      ROT_DEBUG_PRINT("    [0][0] = cos*scale_x = %.6f * %.6f = %.6f\n", cos_r, scale_x, r_uv_to_texture[0][0]);
      ROT_DEBUG_PRINT("    [0][1] = asp_x*sin*scale_x = %.6f * %.6f * %.6f = %.6f\n",
             aspect_x, sin_r, scale_x, r_uv_to_texture[0][1]);
      ROT_DEBUG_PRINT("    [1][0] = -asp_y*sin*scale_y = -%.6f * %.6f * %.6f = %.6f\n",
             aspect_y, sin_r, scale_y, r_uv_to_texture[1][0]);
      ROT_DEBUG_PRINT("    [1][1] = cos*scale_y = %.6f * %.6f = %.6f\n", cos_r, scale_y, r_uv_to_texture[1][1]);

      r_uv_to_texture[3][0] = translate_x + pivot_ss_x - pivot_ss_rot_x;
      r_uv_to_texture[3][1] = translate_y + pivot_ss_y - pivot_ss_rot_y;

      ROT_DEBUG_PRINT("  Translation: trans + pivot_delta = [%.6f, %.6f]\n",
             r_uv_to_texture[3][0], r_uv_to_texture[3][1]);
      ROT_DEBUG_PRINT("--- END ROTATION BRANCH ---\n\n");
    }
    else {
      ROT_DEBUG_PRINT("NO ROTATION - using direct scale and translate\n");
      r_uv_to_texture[0][0] = scale_x;
      r_uv_to_texture[1][1] = scale_y;
      r_uv_to_texture[3][0] = translate_x;
      r_uv_to_texture[3][1] = translate_y;
    }

    ROT_DEBUG_PRINT("Final matrix (4x4):\n");
    ROT_DEBUG_PRINT("[%.6f, %.6f, %.6f, %.6f]\n", r_uv_to_texture[0][0], r_uv_to_texture[0][1], r_uv_to_texture[0][2], r_uv_to_texture[0][3]);
    ROT_DEBUG_PRINT("[%.6f, %.6f, %.6f, %.6f]\n", r_uv_to_texture[1][0], r_uv_to_texture[1][1], r_uv_to_texture[1][2], r_uv_to_texture[1][3]);
    ROT_DEBUG_PRINT("[%.6f, %.6f, %.6f, %.6f]\n", r_uv_to_texture[2][0], r_uv_to_texture[2][1], r_uv_to_texture[2][2], r_uv_to_texture[2][3]);
    ROT_DEBUG_PRINT("[%.6f, %.6f, %.6f, %.6f]\n", r_uv_to_texture[3][0], r_uv_to_texture[3][1], r_uv_to_texture[3][2], r_uv_to_texture[3][3]);
    ROT_DEBUG_PRINT("=== DEBUG END ===\n\n");
  }
};

}  // namespace blender::image_engine
