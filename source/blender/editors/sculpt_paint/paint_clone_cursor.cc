/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "paint_clone_cursor.hh"

#include <cmath>

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BLI_math_constants.h"

#include "BKE_brush.hh"

#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_vertex_format.hh"

#include "UI_view2d.hh"

#include "paint_clone_source.hh"
#include "paint_intern.hh"

namespace blender::ed::sculpt_paint::clone {

/** Square counterpart of #imm_draw_circle_wire_3d, in the outline's own XY plane. */
static void clone_draw_square_wire(const uint pos, const float half_size)
{
  immBegin(GPU_PRIM_LINE_LOOP, 4);
  immVertex3f(pos, -half_size, -half_size, 0.0f);
  immVertex3f(pos, half_size, -half_size, 0.0f);
  immVertex3f(pos, half_size, half_size, 0.0f);
  immVertex3f(pos, -half_size, half_size, 0.0f);
  immEnd();
}

void clone_dashed_program_bind()
{
  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);
  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
  immUniform1i("colors_len", 0); /* "Simple" mode: one color, no second dash color. */
  immUniform1f("dash_width", 8.0f);
  immUniform1f("udash_factor", 0.5f);
}

void clone_dashed_outline_draw(const uint pos, const float radius, const bool is_rect)
{
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.85f);
  if (is_rect) {
    clone_draw_square_wire(pos, radius);
    return;
  }
  /* The dashed shader measures dash phase per segment, so too many short segments break the
   * pattern up into noise (same workaround as the Grease Pencil eraser cursor). */
  imm_draw_circle_wire_3d(pos, 0.0f, 0.0f, radius, 32);
}

void clone_draw_source_cursor(const CloneSourcePoint *source,
                              const uint pos,
                              const float3 *cursor_co_object,
                              const ViewContext &vc,
                              const Paint &paint,
                              const Brush &brush)
{
  if (source == nullptr || !source->is_valid) {
    return;
  }
  if (vc.region == nullptr || vc.rv3d == nullptr || vc.obact == nullptr) {
    return;
  }
  if (!source->frame.valid) {
    /* Without a frame there is no surface plane to lie in and no orientation to show. Drawing a
     * screen-facing disc instead would claim an alignment the sampling does not have. */
    return;
  }

  /* Relative reads at a constant offset from the brush, so a marker left at the picked point
   * would advertise a spot the stroke stopped using. Slide it by how far the brush has travelled
   * since the anchor.
   *
   * Straight object-space translation, while the dab itself displaces the source in UV through
   * #CloneDabTransform.dest_uv_to_source_uv: the two agree wherever the parametrization is
   * uniform between the anchor and here, and drift on a distorted or seam-crossing patch. An
   * approximate marker that moves is still a truer account of the tool than an exact one that
   * claims to be standing still. */
  float3 marker_co = source->co_object;
  if (paint.clone_mode == CLONE_MODE_RELATIVE && source->anchor_valid &&
      cursor_co_object != nullptr)
  {
    marker_co += *cursor_co_object - source->anchor_co_object;
  }

  /* Object-space radius, from the CURRENT brush size, because that is the footprint the next dab
   * will read -- the pick-time radius would go stale the moment the user resizes the brush. */
  const float pixel_radius = BKE_brush_radius_get(&paint, &brush);
  const float radius = paint_calc_object_space_radius(vc, marker_co, pixel_radius);
  if (!(radius > 0.0f)) {
    return;
  }

  /* The marker sits in the frame the samples are taken in: its plane is the surface's, its axes
   * are the ones the footprint is shaped along. So it stays on the mesh under a viewport
   * rotation, and a rectangular footprint is drawn as the rectangle it actually is. */
  const CloneSurfaceFrame &frame = source->frame;
  float4x4 marker_to_object = float4x4::identity();
  marker_to_object.x_axis() = frame.t_screen;
  marker_to_object.y_axis() = frame.b_screen;
  marker_to_object.z_axis() = frame.n_m;
  marker_to_object.location() = marker_co;

  /* Object space, because the caller has already pushed the object matrix. */
  GPU_matrix_push();
  GPU_matrix_mul(marker_to_object.ptr());

  /* Read live, so switching Texture Clip mid-stroke changes the marker at once -- the sampling
   * footprint reads the same field per dab. */
  const bool is_rect = brush.texture_clip_shape == BRUSH_TEXTURE_CLIP_RECTANGLE;
  clone_dashed_outline_draw(pos, radius, is_rect);

  GPU_matrix_pop();
}

void clone_2d_draw_source_cursor(const ImagePaintSettings &settings,
                                 const Paint &paint,
                                 const Brush &brush,
                                 const ARegion &region,
                                 const int2 &canvas_size,
                                 const float2 &cursor_uv,
                                 const float marker_radius_px)
{
  const std::optional<Clone2DSource> source = clone_2d_source_get(settings);
  if (!source.has_value() || !(marker_radius_px > 0.0f)) {
    return;
  }

  /* Relative reads at a constant offset from the brush: slide the marker by the travel since the
   * anchor, so it marks the spot the stroke actually reads from rather than the point that was
   * picked. The offset is taken in UV, the space the dab displaces the source in. */
  float2 marker_uv = source->uv;
  if (paint.clone_mode == CLONE_MODE_RELATIVE && source->anchor_valid) {
    marker_uv += cursor_uv - source->anchor_uv;
  }

  /* Footprint radius in UV, the same measure the dab stamps with (#clone_2d_stroke_dab): brush
   * radius in canvas pixels over the canvas width. */
  const float uv_radius = canvas_size[0] > 0 ?
                              BKE_brush_radius_get(&paint, &brush) / float(canvas_size[0]) :
                              0.0f;

  /* Sampling outside the canvas UV is refused (#clone_2d_stroke_dab skips such dabs; the sampler
   * would wrap), so the preview turns red to say so at a glance -- the fill inside the shape plus
   * the outline. */
  const bool sampling_outside = !(marker_uv[0] >= 0.0f && marker_uv[0] <= 1.0f &&
                                  marker_uv[1] >= 0.0f && marker_uv[1] <= 1.0f);

  /* Every shape point is built in canvas UV and projected through the display frame
   * (#view2d_view_to_region_fl), so the marker turns, zooms and stretches exactly with the
   * canvas: the footprint the stamp paints is UV-locked, and an axis-aligned screen shape stops
   * matching it the moment the canvas is rotated. Region pixels become window pixels with the
   * winrct offset; cursor callbacks draw under the window ortho. */
  const auto project_window = [&](const float2 &uv) {
    float region_xy[2];
    ui::view2d_view_to_region_fl(&region.v2d, uv[0], uv[1], &region_xy[0], &region_xy[1]);
    return float2(region_xy[0] + region.winrct.xmin, region_xy[1] + region.winrct.ymin);
  };

  float2 shape[32];
  int shape_len = 0;
  if (uv_radius > 0.0f) {
    if (brush.texture_clip_shape == BRUSH_TEXTURE_CLIP_RECTANGLE) {
      /* Corners projected individually: under canvas rotation the footprint is a rotated quad on
       * screen, which an axis-aligned square cannot represent (same as
       * #draw_image_uv_custom_region). */
      shape[shape_len++] = project_window(marker_uv + float2(-uv_radius, -uv_radius));
      shape[shape_len++] = project_window(marker_uv + float2(uv_radius, -uv_radius));
      shape[shape_len++] = project_window(marker_uv + float2(uv_radius, uv_radius));
      shape[shape_len++] = project_window(marker_uv + float2(-uv_radius, uv_radius));
    }
    else {
      /* 32 segments: the dashed shader measures dash phase per segment, and more short segments
       * break the pattern into noise (same workaround as #clone_dashed_outline_draw). */
      for (int i = 0; i < 32; i++) {
        const float angle = (2.0f * float(M_PI) * i) / 32;
        shape[shape_len++] = project_window(marker_uv +
                                            uv_radius * float2(std::cos(angle), std::sin(angle)));
      }
    }
  }
  else {
    /* No canvas size resolved yet: fall back to a screen shape at the cursor radius -- exact for
     * a square canvas shown unrotated, approximate otherwise. */
    float marker_region_xy[2];
    ui::view2d_view_to_region_fl(
        &region.v2d, marker_uv[0], marker_uv[1], &marker_region_xy[0], &marker_region_xy[1]);
    const float2 center = float2(marker_region_xy[0] + region.winrct.xmin,
                                 marker_region_xy[1] + region.winrct.ymin);
    if (brush.texture_clip_shape == BRUSH_TEXTURE_CLIP_RECTANGLE) {
      shape[shape_len++] = center + float2(-marker_radius_px, -marker_radius_px);
      shape[shape_len++] = center + float2(marker_radius_px, -marker_radius_px);
      shape[shape_len++] = center + float2(marker_radius_px, marker_radius_px);
      shape[shape_len++] = center + float2(-marker_radius_px, marker_radius_px);
    }
    else {
      for (int i = 0; i < 32; i++) {
        const float angle = (2.0f * float(M_PI) * i) / 32;
        shape[shape_len++] = center + marker_radius_px * float2(std::cos(angle), std::sin(angle));
      }
    }
  }

  /* The caller's uniform-color program is unbound on entry; leave it unbound on exit and let the
   * caller restore it (same contract as the Texture3D marker path). The position attribute goes
   * into a fresh vertex format BEFORE the program bind: #immBindShader packs whatever format is
   * current at bind time, and #immVertexFormat clears it -- adding attributes after the bind
   * leaves an empty format for #immBegin. */
  const uint pos = GPU_vertformat_attr_add(
      immVertexFormat(), "pos", gpu::VertAttrType::SFLOAT_32_32);

  if (sampling_outside) {
    /* Translucent red fill under the outline. Solid program: the dashed one dashes lines only. */
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4f(1.0f, 0.1f, 0.05f, 0.30f);
    immBegin(GPU_PRIM_TRI_FAN, shape_len);
    for (int i = 0; i < shape_len; i++) {
      immVertex2f(pos, shape[i][0], shape[i][1]);
    }
    immEnd();
    immUnbindProgram();
  }

  clone_dashed_program_bind();
  if (sampling_outside) {
    immUniformColor4f(1.0f, 0.15f, 0.1f, 0.95f);
  }
  else {
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.85f);
  }
  GPU_line_width(1.0f);
  immBegin(GPU_PRIM_LINE_LOOP, shape_len);
  for (int i = 0; i < shape_len; i++) {
    immVertex2f(pos, shape[i][0], shape[i][1]);
  }
  immEnd();
  immUnbindProgram();
}

}  // namespace blender::ed::sculpt_paint::clone
