/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Selection masks in the 3D Viewport: the fragment write-back of the floating tools. The mapped
 * writer evaluates a destination-to-source UV map per texel (the affine) and
 * composites the fragments onto the canvas tiles; the commit wrapper owns the shared undo step of
 * a (possibly multi-canvas) commit.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>

#include "BLF_api.hh"
#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_bitmap.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_function_ref.hh"
#include "BLI_index_mask.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_library.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "DRW_engine.hh"
#include "DRW_select_buffer.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_uvedit.hh"
#include "ED_view3d.hh"

#include "GPU_batch.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_vertex_buffer.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../shapes/paint_shape_space.hh"
#include "../paint_intern.hh"
#include "../mesh/mesh_brush_common.hh"
#include "paint_face_selection_mask.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_view3d.hh"
#include "../mesh/paint_image_uv_geom.hh"
#include "../mesh/sculpt_intern.hh"

#include "GEO_reverse_uv_sampler.hh"

namespace blender::ed::sculpt_paint {

/** Destination -> source UV map of one fragment: resolve \a dest_uv to the source UV \a r_src_uv
 * the texel is copied from. False when the point has no source (a UV gap). */
using ImageSelectSrcUvFn = FunctionRef<bool(const SelectionTileFragment &fragment,
                                           const float2 &dest_uv,
                                           float2 &r_src_uv)>;

void image_select_view3d_fragments_write_mapped(
    bContext *C,
    Image *ima,
    const ImageUser &base_iuser,
    const Span<SelectionTileFragment> fragments,
    const ImageSelectSrcUvFn src_uv_for_dest,
    const float2x2 &normal_rotation,
    const bool full_tile_destination,
    const FunctionRef<bool(const float2 &src_uv, float2 &r_dest_uv)> forward_uv,
    const bool local_normal_jacobian)
{
  const bool rotate_normals = local_normal_jacobian ||
                              !image_select_normal_rotation_is_identity(normal_rotation);

  /* Re-apply the cut the lift made originally: #image_select_fragment_commit_with_undo restored
   * the source region to its pre-lift original for the "before" snapshot. Idempotent. */
  image_select_fragment_lift_source(C, ima, base_iuser, fragments);

  ImageUndoStep *us_open = image_select_undo_session_step_get();
  Set<int> snapshotted_tiles;
  for (const SelectionTileFragment &frag : fragments) {
    snapshotted_tiles.add(frag.geom.tile_number);
  }

  for (const SelectionTileFragment &frag : fragments) {
    if (frag.pixels.fragment_ibuf == nullptr) {
      continue;
    }
    const float2 src_tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
    const float2 frag_origin_uv = src_tile_uv +
                                  float2(frag.geom.origin_px) / float2(frag.geom.tile_size_px);
    const float2 frag_size_uv = float2(frag.geom.size_px) / float2(frag.geom.tile_size_px);

    const float *fmask = frag.pixels.fragment_mask_ibuf ?
                             frag.pixels.fragment_mask_ibuf->float_data() :
                             nullptr;
    const ImBuf *blend_mask = frag.edge_policy.use_outward_feather ?
                                  frag.preview.fragment_blend_mask_ibuf :
                                  nullptr;

    /* Destination region: the fragment's full UV footprint through the caller's forward map,
     * clipped to the tile, or the whole tile when no forward map is given. */
    bool have_dest_bounds = !full_tile_destination && bool(forward_uv);
    float2 dest_min(FLT_MAX, FLT_MAX);
    float2 dest_max(-FLT_MAX, -FLT_MAX);
    if (have_dest_bounds) {
      for (const int i : IndexRange(4)) {
        const float2 corner_src(frag_origin_uv + float2((i & 1) ? frag_size_uv.x : 0.0f,
                                                        (i & 2) ? frag_size_uv.y : 0.0f));
        float2 corner_dst;
        if (!forward_uv(corner_src, corner_dst)) {
          have_dest_bounds = false;
          break;
        }
        dest_min = math::min(dest_min, corner_dst);
        dest_max = math::max(dest_max, corner_dst);
      }
    }

    for (ImageTile *tile : ListBaseWrapper<ImageTile>(ima->tiles)) {
      const float2 dst_tile_uv = image_select_udim_tile_uv_origin(tile->tile_number);

      ImageUser tile_iuser = base_iuser;
      tile_iuser.tile = tile->tile_number;
      void *lock = nullptr;
      ImBuf *ibuf = BKE_image_acquire_ibuf(ima, &tile_iuser, &lock);
      if (ibuf == nullptr || (!ibuf->float_buffer.data && !ibuf->byte_buffer.data)) {
        if (ibuf != nullptr) {
          BKE_image_release_ibuf(ima, ibuf, lock);
        }
        continue;
      }

      if (us_open != nullptr && !snapshotted_tiles.contains(tile->tile_number)) {
        ED_image_undo_push(ima, ibuf, &tile_iuser, us_open);
        ED_image_undo_capture_selection_mask(ima, tile->tile_number);
        snapshotted_tiles.add(tile->tile_number);
      }

      const float dst_w = float(ibuf->x);
      const float dst_h = float(ibuf->y);
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      const bool is_float = ibuf->float_data() != nullptr;

      int dp_x0 = 0;
      int dp_y0 = 0;
      int dp_x1 = ibuf->x;
      int dp_y1 = ibuf->y;
      if (have_dest_bounds) {
        dp_x0 = std::max(0, int(std::floor((dest_min.x - dst_tile_uv.x) * dst_w)));
        dp_y0 = std::max(0, int(std::floor((dest_min.y - dst_tile_uv.y) * dst_h)));
        dp_x1 = std::min(int(dst_w), int(std::ceil((dest_max.x - dst_tile_uv.x) * dst_w)));
        dp_y1 = std::min(int(dst_h), int(std::ceil((dest_max.y - dst_tile_uv.y) * dst_h)));
      }
      const int64_t dp_rows = std::max(0, dp_y1 - dp_y0);
      if (dp_rows <= 0) {
        BKE_image_release_ibuf(ima, ibuf, lock);
        continue;
      }

      if (is_float && frag.pixels.fragment_ibuf->float_data() != nullptr) {
        const int channels = ibuf->channels ? ibuf->channels : 4;
        float *dst_data = ibuf->float_data_for_write();
        const int frag_w = frag.geom.size_px.x;
        const int frag_h = frag.geom.size_px.y;
        threading::parallel_for(IndexRange(dp_y0, dp_rows), 16, [&](const IndexRange range) {
          for (const int64_t py : range) {
            for (int px = dp_x0; px < dp_x1; px++) {
              const float2 dest_uv = dst_tile_uv +
                                     float2((float(px) + 0.5f) / dst_w, (float(py) + 0.5f) / dst_h);
              float2 src_uv;
              if (!src_uv_for_dest(frag, dest_uv, src_uv)) {
                continue;
              }
              const float2 f = (src_uv - frag_origin_uv) / frag_size_uv * float2(frag_w, frag_h);
              if (f.x < 0.0f || f.y < 0.0f || f.x >= float(frag_w) || f.y >= float(frag_h)) {
                continue;
              }
              float m = 1.0f;
              if (blend_mask != nullptr) {
                m = image_select_sample_mask_bilinear(blend_mask, f.x, f.y);
              }
              else if (fmask != nullptr) {
                if (fmask[int(f.y) * frag_w + int(f.x)] <= SELECTION_MASK_THRESHOLD) {
                  continue;
                }
              }
              if (m <= 0.001f) {
                continue;
              }

              float4 frag_color(0.0f, 0.0f, 0.0f, 1.0f);
              if (!image_select_fragment_sample_bilinear(frag, f, channels, frag_color)) {
                continue;
              }
              if (rotate_normals) {
                float2x2 pixel_rotation = normal_rotation;
                if (local_normal_jacobian) {
                  float2 src_du, src_dv;
                  if (src_uv_for_dest(frag, dest_uv + float2(1.0f / dst_w, 0.0f), src_du) &&
                      src_uv_for_dest(frag, dest_uv + float2(0.0f, 1.0f / dst_h), src_dv))
                  {
                    float2x2 jacobian;
                    jacobian[0] = (src_du - src_uv) * dst_w;
                    jacobian[1] = (src_dv - src_uv) * dst_h;
                    pixel_rotation = image_select_normal_rotation(jacobian);
                  }
                }
                image_select_normal_color_rotate(frag_color, pixel_rotation);
              }
              float *dst_px = dst_data + (py * int(dst_w) + px) * channels;
              if (channels >= 4) {
                /* The map carries its own alpha, so the fragment composites "over" it: a layer map
                 * is transparent outside what was painted, and a lerp would darken the RGB and
                 * square the alpha there. */
                const float4 dst_color(dst_px[0], dst_px[1], dst_px[2], dst_px[3]);
                const float4 out = image_select_composite_over(dst_color, frag_color, m);
                for (int c = 0; c < 4; c++) {
                  dst_px[c] = out[c];
                }
                continue;
              }
              if (m <= 0.001f) {
                continue;
              }
              for (int c = 0; c < channels; c++) {
                dst_px[c] = (1.0f - m) * dst_px[c] + m * frag_color[c];
              }
            }
          }
        });
      }
      else if (!is_float && ibuf->byte_data() != nullptr &&
               frag.pixels.fragment_ibuf->byte_data() != nullptr)
      {
        uint8_t *dst_data = ibuf->byte_data_for_write();
        const int frag_w = frag.geom.size_px.x;
        const int frag_h = frag.geom.size_px.y;
        threading::parallel_for(IndexRange(dp_y0, dp_rows), 16, [&](const IndexRange range) {
          for (const int64_t py : range) {
            for (int px = dp_x0; px < dp_x1; px++) {
              const float2 dest_uv = dst_tile_uv +
                                     float2((float(px) + 0.5f) / dst_w, (float(py) + 0.5f) / dst_h);
              float2 src_uv;
              if (!src_uv_for_dest(frag, dest_uv, src_uv)) {
                continue;
              }
              const float2 f = (src_uv - frag_origin_uv) / frag_size_uv * float2(frag_w, frag_h);
              if (f.x < 0.0f || f.y < 0.0f || f.x >= float(frag_w) || f.y >= float(frag_h)) {
                continue;
              }
              float m = 1.0f;
              if (blend_mask != nullptr) {
                m = image_select_sample_mask_bilinear(blend_mask, f.x, f.y);
              }
              else if (fmask != nullptr) {
                if (fmask[int(f.y) * frag_w + int(f.x)] <= SELECTION_MASK_THRESHOLD) {
                  continue;
                }
              }
              if (m <= 0.001f) {
                continue;
              }

              float4 frag_color(0.0f, 0.0f, 0.0f, 1.0f);
              if (!image_select_fragment_sample_bilinear(frag, f, 4, frag_color)) {
                continue;
              }
              if (rotate_normals) {
                float2x2 pixel_rotation = normal_rotation;
                if (local_normal_jacobian) {
                  float2 src_du, src_dv;
                  if (src_uv_for_dest(frag, dest_uv + float2(1.0f / dst_w, 0.0f), src_du) &&
                      src_uv_for_dest(frag, dest_uv + float2(0.0f, 1.0f / dst_h), src_dv))
                  {
                    float2x2 jacobian;
                    jacobian[0] = (src_du - src_uv) * dst_w;
                    jacobian[1] = (src_dv - src_uv) * dst_h;
                    pixel_rotation = image_select_normal_rotation(jacobian);
                  }
                }
                image_select_normal_color_rotate(frag_color, pixel_rotation);
              }
              uint8_t *dst_px = dst_data + (py * int(dst_w) + px) * 4;
              /* "Over" like the float path (see #image_select_composite_over). */
              const float4 dst_color(float(dst_px[0]) / 255.0f,
                                     float(dst_px[1]) / 255.0f,
                                     float(dst_px[2]) / 255.0f,
                                     float(dst_px[3]) / 255.0f);
              const float4 out = image_select_composite_over(dst_color, frag_color, m);
              for (int c = 0; c < 4; c++) {
                /* Round rather than truncate (truncation biases the feathered rim down and
                 * compounds across repeated edits; matches the 2D write-back). */
                dst_px[c] = uint8_t(std::clamp(out[c] * 255.0f + 0.5f, 0.0f, 255.0f));
              }
            }
          }
        });
      }

      BKE_image_mark_dirty(ima, ibuf);

      /* Destination selection mask: nearest sample of the fragment mask through the inverse
       * transform (binary). Rows are independent, so they blend in parallel. */
      ImBuf *mask = BKE_image_paint_selection_mask_lookup(ima, tile->tile_number);
      if (mask != nullptr && mask->float_data_for_write() != nullptr) {
        float *mdata = mask->float_data_for_write();
        const int py_lo = std::max(dp_y0, 0);
        const int py_hi = std::min(dp_y1, mask->y);
        const int px_lo = std::max(dp_x0, 0);
        const int px_hi = std::min(dp_x1, mask->x);
        if (py_hi > py_lo && px_hi > px_lo) {
          threading::parallel_for(
              IndexRange(py_lo, py_hi - py_lo), 64, [&](const IndexRange rows) {
                for (const int py : rows) {
                  for (int px = px_lo; px < px_hi; px++) {
                    const float2 dest_uv = dst_tile_uv + float2((float(px) + 0.5f) / dst_w,
                                                                (float(py) + 0.5f) / dst_h);
                    float2 src_uv;
                    if (!src_uv_for_dest(frag, dest_uv, src_uv)) {
                      continue;
                    }
                    const float2 f = (src_uv - frag_origin_uv) / frag_size_uv *
                                     float2(frag.geom.size_px);
                    if (f.x < 0.0f || f.y < 0.0f || f.x >= float(frag.geom.size_px.x) ||
                        f.y >= float(frag.geom.size_px.y))
                    {
                      continue;
                    }
                    mdata[py * mask->x + px] =
                        fmask ? fmask[int(f.y) * frag.geom.size_px.x + int(f.x)] : 1.0f;
                  }
                }
              });
        }
      }

      rcti dirty;
      BLI_rcti_init(&dirty, dp_x0, dp_x1, dp_y0, dp_y1);
      BKE_image_partial_update_mark_region(ima, tile, ibuf, &dirty);

      BKE_image_release_ibuf(ima, ibuf, lock);
    }
  }

  /* The canvas is displayed through cached GPU textures (the 3D paint material and the Image
   * Editor); the partial-update marks alone did not refresh them after the fragment write, so the
   * moved fragment appeared lost. Same call as the Image Editor's #image_select_move_commit. */
  BKE_image_free_gputextures(ima);
  DEG_id_tag_update(&ima->id, 0);
  WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, ima);
}

void image_select_view3d_fragments_write_final(bContext *C,
                                               Image *ima,
                                               const ImageUser &base_iuser,
                                               const Span<SelectionTileFragment> fragments,
                                               const float2x2 &linear_uv,
                                               const float2 &pivot_uv,
                                               const float2 &translation_uv,
                                               const bool is_normal_map)
{
  const float2x2 normal_rotation = is_normal_map ? image_select_normal_rotation(linear_uv) :
                                                   float2x2::identity();

  bool invert_ok = false;
  const float2x2 linear_inv = math::invert(linear_uv, invert_ok);
  if (!invert_ok) {
    return;
  }

  /* The callables are named locals, not FunctionRefs bound to temporaries: a FunctionRef only
   * borrows the callable, so it must outlive the call. */
  const auto src_uv_for_dest = [linear_inv, pivot_uv, translation_uv](
                                   const SelectionTileFragment & /*fragment*/,
                                   const float2 &dest_uv,
                                   float2 &r_src_uv) {
    r_src_uv = pivot_uv + linear_inv * (dest_uv - pivot_uv - translation_uv);
    return true;
  };
  const auto forward_uv = [linear_uv, pivot_uv, translation_uv](const float2 &src_uv,
                                                                float2 &r_dest_uv) {
    r_dest_uv = pivot_uv + linear_uv * (src_uv - pivot_uv) + translation_uv;
    return true;
  };

  image_select_view3d_fragments_write_mapped(
      C, ima, base_iuser, fragments, src_uv_for_dest, normal_rotation, false, forward_uv, false);
}

void image_select_view3d_commit_targets_with_undo(
    bContext *C,
    const char *undo_label,
    const Span<ImagePaintSelectionTarget> targets,
    const Span<Span<SelectionTileFragment>> fragments_per_target,
    const FunctionRef<void(Image &ima,
                           const ImageUser &iuser,
                           Span<SelectionTileFragment> fragments)> write_final_per_target)
{
  BLI_assert(targets.size() == fragments_per_target.size());
  if (targets.is_empty()) {
    return;
  }

  if (targets.size() == 1) {
    /* Single canvas: the self-contained wrapper owns its whole undo step. */
    image_select_fragment_commit_with_undo(C,
                                           targets[0].image,
                                           targets[0].iuser,
                                           fragments_per_target[0],
                                           undo_label,
                                           [&]() {
                                             write_final_per_target(*targets[0].image,
                                                                    targets[0].iuser,
                                                                    fragments_per_target[0]);
                                           });
    image_paint_selection_targets_update(C, targets);
    return;
  }

  /* Multi-canvas commit (a Material canvas lifts one fragment set per PBR map): restore the
   * sources so every "before" snapshot is the pre-edit canvas rather than the lifted hole,
   * snapshot ALL targets into ONE open undo step, and only then write. Per-target undo steps
   * would restore the canvases one Ctrl+Z at a time, leaving the material half-edited in between
   * (the Image Editor's warp does the same in #image_select_warp_commit_material_targets). */
  for (const int i : targets.index_range()) {
    image_select_fragment_restore_source(
        C, targets[i].image, targets[i].iuser, fragments_per_target[i]);
  }

  const int first_tile = fragments_per_target[0].first().geom.tile_number;
  ED_imapaint_clear_partial_redraw();
  {
    ImageUser first_iuser = targets[0].iuser;
    first_iuser.tile = first_tile;
    void *lock = nullptr;
    ImBuf *ibuf = BKE_image_acquire_ibuf(targets[0].image, &first_iuser, &lock);
    if (ibuf != nullptr) {
      ED_image_undo_push_begin_with_image(undo_label, targets[0].image, ibuf, &first_iuser);
      BKE_image_release_ibuf(targets[0].image, ibuf, lock);
    }
    else {
      ED_image_undo_push_begin(undo_label, PaintMode::Texture2D);
    }
    ED_image_undo_capture_selection_mask(targets[0].image, first_tile);
  }

  /* Register every further fragment tile's "before" state into the same open step. The step was
   * opened just above in this same call, so it is still open here: no other operator can have run
   * in between (the same reasoning #image_select_fragment_commit_with_undo relies on). The
   * write-backs may register further tiles they reach through #image_select_undo_session_step_get
   * themselves. */
  ImageUndoStep *us_open = image_select_undo_session_step_get();
  if (us_open != nullptr) {
    for (const int i : targets.index_range()) {
      for (const SelectionTileFragment &frag : fragments_per_target[i]) {
        if (i == 0 && frag.geom.tile_number == first_tile) {
          /* Already anchored by the push-begin above. */
          continue;
        }
        ImageUser tile_iuser = targets[i].iuser;
        tile_iuser.tile = frag.geom.tile_number;
        void *lock = nullptr;
        ImBuf *ibuf = BKE_image_acquire_ibuf(targets[i].image, &tile_iuser, &lock);
        if (ibuf != nullptr) {
          ED_image_undo_push(targets[i].image, ibuf, &tile_iuser, us_open);
          BKE_image_release_ibuf(targets[i].image, ibuf, lock);
        }
        ED_image_undo_capture_selection_mask(targets[i].image, frag.geom.tile_number);
      }
    }
  }

  for (const int i : targets.index_range()) {
    write_final_per_target(*targets[i].image, targets[i].iuser, fragments_per_target[i]);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
}

}  // namespace blender::ed::sculpt_paint
