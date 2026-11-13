/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Transform Selection in the 3D Viewport: the POST_VIEW fragment preview and the POST_PIXEL
 * outline / handle overlay.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_listbase_wrapper.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_rotation.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "DEG_depsgraph_query.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_library.hh"

#include "ED_image.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_floating.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_move_intern.hh"
#include "paint_image_select_transform_view3d_intern.hh"
#include "paint_image_select_view3d.hh"
#include "../mesh/sculpt_intern.hh"

#include "BKE_mesh.hh"
#include "BKE_bvhutils.hh"
#include "BKE_paint_layers.hh"
#include "BKE_screen.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Draw (POST_VIEW: fragment pixels on the surface)
 * \{ */

/**
 * The on-surface fragment preview: every fragment's display buffer drawn where the commit will
 * write it, through the current affine transform. The move preview is the identity-linear special
 * case of this; here the texture coordinates map each mesh triangle's destination UV back to the
 * fragment's source space through the inverse transform.
 */
void transform3d_draw_texture(const bContext *C, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectTransform3DState *>(arg);
  if (state == nullptr || state->anchor.object == nullptr || !state->surface_map_ref().sampler) {
    return;
  }
  if (state->mode == image_select_v3d::Transform3DContentMode::Decal) {
    /* A step-back or restore put the decal frame back outside a gesture. This POST_VIEW callback
     * runs before the POST_PIXEL one that would re-sync, so the patch is brought up to date here
     * or the preview would show the stale one until the next redraw. */
    if (state->decal_frame_dirty && C != nullptr) {
      transform3d_decal_frame_sync(C, *state);
    }
    /* The surface patch carries the stamp: it is drawn at the real surface positions, so the
     * affine texture path below (UV-space triangles) does not apply. */
    transform3d_decal_draw_preview(region, *state);
    return;
  }
  if (state->preview_textures.size() != state->fragments.size()) {
    state->preview_textures.resize(state->fragments.size());
  }
  /* A Material canvas lifts one fragment set per PBR map: drawing them all flat would stack the
   * maps. The composite shades them together; the channels it cannot shade are shown one at a
   * time (the user-cycled target). */
  const int target_i = state->preview_target_resolved();
  if (target_i >= state->fragments.size()) {
    return;
  }
  const float2x2 linear = state->uv_linear;
  const float2 pivot = state->uv_pivot;
  const float2 translation = state->uv_translation;
  bool invert_ok = false;
  const float2x2 linear_inv = math::invert(linear, invert_ok);
  if (!invert_ok) {
    return;
  }
  const auto src_uv_of = [&](const float2 &dest_uv) {
    return pivot + linear_inv * (dest_uv - pivot - translation);
  };
  /* The preview triangles lie exactly on the mesh surface: without a depth offset the shared
   * pixels z-fight and the texture shows through only in patches. */
  if (region != nullptr && region->regiondata != nullptr) {
    ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 1.0f);
  }
  if (target_i < 0) {
    image_select_view3d_draw_pbr_composite(
        *state->anchor.object,
        state->surface_map_ref(),
        region,
        state->targets,
        state->fragments,
        state->preview_textures,
        state->pbr_default_textures,
        [&](const float2 &src_uv) {
          return image_select_v3d::transform_uv(linear, pivot, translation, src_uv);
        },
        src_uv_of);
    if (region != nullptr && region->regiondata != nullptr) {
      ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 0.0f);
    }
    return;
  }
  const Vector<SelectionTileFragment> &per_target = state->fragments[target_i];
  Vector<gpu::Texture *> &textures = state->preview_textures[target_i];
  if (textures.size() != per_target.size()) {
    textures.resize(per_target.size(), nullptr);
  }
  for (const int frag_i : per_target.index_range()) {
    const SelectionTileFragment &frag = per_target[frag_i];
    if (frag.preview.fragment_display_ibuf == nullptr) {
      continue;
    }
    if (textures[frag_i] == nullptr) {
      textures[frag_i] = IMB_create_gpu_texture(
          "Transform3DFragment", frag.preview.fragment_display_ibuf, true, false, false);
      if (textures[frag_i] != nullptr) {
        /* Linear, not nearest: see #move3d_draw (heavy minification on the surface). */
        GPU_texture_filter_mode(textures[frag_i], true);
      }
    }
    const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
    const float2 tile_size = float2(frag.geom.tile_size_px);
    const float2 src_origin_uv = tile_uv + float2(frag.geom.origin_px) / tile_size;
    const float2 src_size_uv = float2(frag.geom.size_px) / tile_size;
    /* Destination rect of this fragment: its four source corners through the forward transform. */
    float2 dest_min(FLT_MAX, FLT_MAX);
    float2 dest_max(-FLT_MAX, -FLT_MAX);
    for (const int i : IndexRange(4)) {
      const float2 corner_src = src_origin_uv +
                                float2((i & 1) ? src_size_uv.x : 0.0f,
                                       (i & 2) ? src_size_uv.y : 0.0f);
      const float2 corner_dest = image_select_v3d::transform_uv(linear,
                                                                pivot,
                                                                translation,
                                                                corner_src);
      dest_min = math::min(dest_min, corner_dest);
      dest_max = math::max(dest_max, corner_dest);
    }
    image_select_view3d_draw_uv_texture_ext(*state->anchor.object,
                                            state->surface_map_ref(),
                                            textures[frag_i],
                                            dest_min,
                                            dest_max,
                                            [&](const float2 &dest_uv) {
                                              return (src_uv_of(dest_uv) - src_origin_uv) /
                                                     src_size_uv;
                                            });
  }
  if (region != nullptr && region->regiondata != nullptr) {
    ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 0.0f);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Draw (POST_PIXEL: outline + handles)
 * \{ */

void transform3d_draw(const bContext *C, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectTransform3DState *>(arg);
  if (state == nullptr || region == nullptr || state->anchor.object == nullptr) {
    return;
  }
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region->regiondata);
  if (rv3d == nullptr) {
    return;
  }
  /* An undo step / pivot drag invalidates the surface frame; re-sample it before placing the
   * handles so the gizmo never draws in a plane the fragment has left. */
  if (state->mode == image_select_v3d::Transform3DContentMode::Decal &&
      state->decal_frame_dirty)
  {
    /* A step-back or restore put the decal frame back outside a gesture: the draw callback is the
     * one caller that always has a context, so the frame and the patch re-sync here. */
    transform3d_decal_frame_sync(C, *state);
  }
  if (!state->display_frame_valid) {
    transform3d_refresh_display_anchor(C, *state, true);
  }
  transform3d_refresh_screen(*state, *region);

  /* Fragment outline (transformed), drawn in region px with the dashed shader. */
  Vector<Vector<float2>> screen_polylines;
  if (state->surface_map_ref().sampler && !state->hide_outline) {
    const float2x2 linear = state->uv_linear;
    const float2 pivot = state->uv_pivot;
    const float2 translation = state->uv_translation;
    for (const Vector<float2> &outline : state->outline_uv) {
      Vector<float2> screen;
      screen.resize(outline.size());
      int drawn = 0;
      for (const int i : outline.index_range()) {
        const float2 uv = image_select_v3d::transform_uv(linear, pivot, translation, outline[i]);
        if (transform3d_uv_to_screen(*state, *region, uv, screen[i])) {
          drawn++;
        }
      }
      if (drawn >= 2) {
        screen_polylines.append(std::move(screen));
      }
    }
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_NONE);
  const uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos",
                                           gpu::VertAttrType::SFLOAT_32_32_32);

  if (!screen_polylines.is_empty()) {
    immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);
    float viewport_size[4];
    GPU_viewport_size_get_f(viewport_size);
    immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
    immUniform1i("colors_len", 2);
    immUniform4f("color", 1.0f, 0.85f, 0.0f, 0.9f);
    immUniform4f("color2", 1.0f, 1.0f, 1.0f, 1.0f);
    immUniform1f("dash_width", 8.0f);
    immUniform1f("udash_factor", 0.5f);
    /* The inputs are segment pairs (#image_select_mask_boundary_segments), not connected
     * polylines: chaining consecutive points joins unrelated boundary segments with long lines
     * across the fragment (the move tool's outlines go through the same contract). */
    int64_t total = 0;
    for (const Vector<float2> &poly : screen_polylines) {
      total += int64_t(poly.size());
    }
    immBegin(GPU_PRIM_LINES, uint(total));
    for (const Vector<float2> &poly : screen_polylines) {
      for (int64_t i = 0; i + 1 < poly.size(); i += 2) {
        immVertex3f(pos, poly[i].x, poly[i].y, 0.0f);
        immVertex3f(pos, poly[i + 1].x, poly[i + 1].y, 0.0f);
      }
    }
    immEnd();
    immUnbindProgram();
  }

  /* Handles: the cage, corner / edge squares, the rotate handle on a stem and the pivot ring. */
  if (state->screen_valid) {
    const float s = 5.0f * UI_SCALE_FAC;
    const Transform3DHandle emphasized = state->is_dragging ? state->active_handle :
                                                              state->hover_handle;
    const float4 fill_color(1.0f, 1.0f, 1.0f, 0.95f);
    const float4 fill_color_hot(1.0f, 0.65f, 0.1f, 1.0f);
    const float4 edge_color(0.05f, 0.05f, 0.05f, 0.9f);

    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);

    /* Cage outline, a dark underlay one pixel off keeps it readable on any surface color. Each
     * edge is the surface-resolved polyline, so the cage visibly bends over a face edge instead of
     * cutting straight through it. */
    const auto draw_cage = [&](const float2 &offset) {
      for (const int i : IndexRange(4)) {
        const Vector<float2> &poly = state->screen_cage[i];
        if (poly.size() < 2) {
          continue;
        }
        immBegin(GPU_PRIM_LINE_STRIP, uint(poly.size()));
        for (const float2 &p : poly) {
          immVertex3f(pos, p.x + offset.x, p.y + offset.y, 0.0f);
        }
        immEnd();
      }
    };
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.45f);
    draw_cage(float2(1.0f, -1.0f));
    immUniformColor4f(1.0f, 1.0f, 1.0f, 0.9f);
    draw_cage(float2(0.0f));

    /* Stem between the top edge and the rotate handle. */
    immBegin(GPU_PRIM_LINES, 2);
    immVertex3f(pos, state->screen_edges[2].x, state->screen_edges[2].y, 0.0f);
    immVertex3f(pos, state->screen_rotate.x, state->screen_rotate.y, 0.0f);
    immEnd();

    const auto draw_square = [&](const float2 &p, const float half, const bool hot) {
      immUniformColor4fv(hot ? fill_color_hot : fill_color);
      immBegin(GPU_PRIM_TRI_FAN, 4);
      immVertex3f(pos, p.x - half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y + half, 0.0f);
      immVertex3f(pos, p.x - half, p.y + half, 0.0f);
      immEnd();
      immUniformColor4fv(edge_color);
      immBegin(GPU_PRIM_LINE_LOOP, 4);
      immVertex3f(pos, p.x - half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y - half, 0.0f);
      immVertex3f(pos, p.x + half, p.y + half, 0.0f);
      immVertex3f(pos, p.x - half, p.y + half, 0.0f);
      immEnd();
    };
    constexpr int CIRCLE_SEGMENTS = 20;
    const auto draw_circle = [&](const float2 &p,
                                 const float radius,
                                 const bool filled,
                                 const float4 &color) {
      if (filled) {
        immUniformColor4fv(color);
        immBegin(GPU_PRIM_TRI_FAN, CIRCLE_SEGMENTS + 2);
        immVertex3f(pos, p.x, p.y, 0.0f);
        for (const int i : IndexRange(CIRCLE_SEGMENTS + 1)) {
          const float angle = float(i) / float(CIRCLE_SEGMENTS) * float(2.0 * M_PI);
          immVertex3f(pos, p.x + std::cos(angle) * radius, p.y + std::sin(angle) * radius, 0.0f);
        }
        immEnd();
      }
      immUniformColor4fv(filled ? edge_color : color);
      immBegin(GPU_PRIM_LINE_LOOP, CIRCLE_SEGMENTS);
      for (const int i : IndexRange(CIRCLE_SEGMENTS)) {
        const float angle = float(i) / float(CIRCLE_SEGMENTS) * float(2.0 * M_PI);
        immVertex3f(pos, p.x + std::cos(angle) * radius, p.y + std::sin(angle) * radius, 0.0f);
      }
      immEnd();
    };

    for (const int i : IndexRange(4)) {
      draw_square(state->screen_edges[i],
                  s * 0.8f,
                  emphasized == Transform3DHandle(int(Transform3DHandle::Edge0) + i));
    }
    for (const int i : IndexRange(4)) {
      draw_square(state->screen_corners[i],
                  s,
                  emphasized == Transform3DHandle(int(Transform3DHandle::Corner0) + i));
    }
    draw_circle(state->screen_rotate,
                s * 1.2f,
                true,
                emphasized == Transform3DHandle::Rotate ? fill_color_hot : fill_color);

    /* Pivot: a ring with a cross, blue so it never reads as a scale handle. */
    const float4 pivot_color = emphasized == Transform3DHandle::Pivot ?
                                   fill_color_hot :
                                   float4(0.2f, 0.6f, 1.0f, 1.0f);
    draw_circle(state->screen_pivot, s * 1.2f, false, pivot_color);
    immUniformColor4fv(pivot_color);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex3f(pos, state->screen_pivot.x - s * 0.7f, state->screen_pivot.y, 0.0f);
    immVertex3f(pos, state->screen_pivot.x + s * 0.7f, state->screen_pivot.y, 0.0f);
    immVertex3f(pos, state->screen_pivot.x, state->screen_pivot.y - s * 0.7f, 0.0f);
    immVertex3f(pos, state->screen_pivot.x, state->screen_pivot.y + s * 0.7f, 0.0f);
    immEnd();
    immUnbindProgram();
  }

  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_blend(GPU_BLEND_NONE);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
