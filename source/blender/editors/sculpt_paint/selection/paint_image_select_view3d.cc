/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Selection masks in the 3D Viewport: anchor/plane mapping, the per-texel rasterizer that
 * projects mesh texels onto the anchor plane and writes the UV-space masks, the shared gesture
 * apply, the all/none/invert and gesture operators, the floating-session slot, and the 3D
 * overlay (marching-ants outline projected onto the surface).
 *
 * The mask storage, the edge policy, the undo capture and the Image-Editor borrow guard are the
 * Image Editor's own (BKE_image_paint_selection.hh + paint_image_select_intern.hh); this file
 * only adds the 3D projection half. See paint_image_select_view3d.hh for the flow.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

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

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Drag threshold (region px) under which a gesture counts as a simple click (deselect). */
constexpr int SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX = 3;
/** Lasso/polyline minimum point spacing (region px). */
constexpr float SELECT_VIEW3D_PATH_MIN_STEP_PX = 2.0f;
/** Marching-ants dash width (matching the Image Editor overlay). */
constexpr float SELECT_VIEW3D_DASH_WIDTH = 8.0f;
/** Extra rejection depth floor, object units: keeps flat selections usable on tiny scales. */
constexpr float SELECT_VIEW3D_MIN_DEPTH = 1e-4f;
/** Rejection depth along the anchor normal, as a fraction of the gesture's larger half-axis. */
constexpr float SELECT_VIEW3D_MAX_DEPTH_FACTOR = 0.5f;
/** Upper bound of the overlay's triangle count (a normal selection needs far fewer). */
constexpr int64_t SELECT_VIEW3D_OUTLINE_MAX_TRIS = 200000;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Small helpers
 * \{ */

namespace image_select_v3d {

/** Object-space corner \a corner (0..7) of an AABB (the local twin of the shapes' helper). */
inline float3 bounds_corner(const Bounds<float3> &bounds, const int corner)
{
  return float3((corner & 1) ? bounds.max.x : bounds.min.x,
                (corner & 2) ? bounds.max.y : bounds.min.y,
                (corner & 4) ? bounds.max.z : bounds.min.z);
}

/** Raw (acceptance-rule-free) projection of a point onto the anchor plane, for bounds culling.
 * False in View mode for a point behind the view, which the caller must not cull. */
inline bool project_raw(const ImageSelectView3DAnchor &anchor,
                        const float3 &co_object,
                        float2 &r_px)
{
  if (anchor.screen) {
    return anchor.project_screen(co_object, r_px);
  }
  const float3 d = co_object - anchor.anchor.co;
  r_px = float2(math::dot(d, anchor.frame.tangent), math::dot(d, anchor.frame.bitangent)) *
         anchor.anchor.px_per_unit;
  return true;
}

struct RasterTLS {
  Vector<int> touched_faces;
};

/** The tile's image buffer for \a iuser pinned to \a tile_number. */
ImBuf *tile_ibuf_get(Image *image, const ImageUser &base_iuser, const int tile_number)
{
  ImageUser iuser = base_iuser;
  iuser.tile = tile_number;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, &lock);
  BKE_image_release_ibuf(image, ibuf, lock);
  return ibuf;
}

}  // namespace image_select_v3d

/** \} */

/* -------------------------------------------------------------------- */
/** \name Polls
 * \{ */

static bool view3d_object_valid(bContext *C, const Object *ob)
{
  if (ob == nullptr || ob->type != OB_MESH || ob->data == nullptr) {
    return false;
  }
  const Scene *scene = CTX_data_scene(C);
  return scene != nullptr && scene->toolsettings != nullptr;
}

bool image_paint_selection_view3d_poll(bContext *C)
{
  if (CTX_data_mode_enum(C) != CTX_MODE_SCULPT) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  if (CTX_wm_region_view3d(C) == nullptr) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if (!view3d_object_valid(C, ob)) {
    return false;
  }
  /* Resolve once: in Material mode this walks the material nodes. */
  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C,
                                                                                            *ob);
  if (targets.is_empty()) {
    return false;
  }
  for (const ImagePaintSelectionTarget &target : targets) {
    const Image *image = target.image;
    if (image == nullptr || !ID_IS_EDITABLE(image) || ID_IS_OVERRIDE_LIBRARY(image)) {
      return false;
    }
  }
  if (image_select_view3d_session_active() != nullptr) {
    return false;
  }
  if (image_select_view3d_canvas_borrowed_elsewhere(C, targets)) {
    return false;
  }
  return true;
}

bool image_paint_selection_view3d_tool_poll(bContext *C)
{
  /* A live session of this Viewport is polled through on purpose: the floating tools' and the
   * gestures' invoke callbacks handle it (re-drag inside the fragment, commit outside). The
   * strict poll below rejects, among others, any live session, so the two cases never overlap. */
  const PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  if (session != nullptr) {
    return session->owner_v3d == CTX_wm_view3d(C);
  }
  return image_paint_selection_view3d_poll(C);
}

bool image_paint_selection_view3d_floating_poll(bContext *C)
{
  if (CTX_data_mode_enum(C) != CTX_MODE_SCULPT) {
    return false;
  }
  const PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  if (session == nullptr) {
    return false;
  }
  return session->owner_v3d == CTX_wm_view3d(C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Targets and undo
 * \{ */

Vector<ImagePaintSelectionTarget> image_paint_selection_view3d_targets_get(bContext *C,
                                                                          Object &ob)
{
  Vector<ImagePaintSelectionTarget> targets;
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return targets;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;

  Image *canvas_image = nullptr;
  ImageUser canvas_iuser = {};
  ImageUser *canvas_iuser_ptr = nullptr;
  BKE_paint_canvas_image_get(&paint_mode, &ob, &canvas_image, &canvas_iuser_ptr);
  if (canvas_iuser_ptr != nullptr) {
    canvas_iuser = *canvas_iuser_ptr;
  }

  if (paint_mode.canvas_source != PAINT_CANVAS_SOURCE_MATERIAL) {
    if (canvas_image != nullptr) {
      targets.append({canvas_image, canvas_iuser});
    }
    return targets;
  }

  const Brush *brush = BKE_paint_brush(&scene->toolsettings->imapaint.paint);
  const BrushMaterialPaint *brush_paint = brush ? brush->material_paint : nullptr;
  const Vector<PaintMaterialImageTarget> material_targets = BKE_paint_material_image_targets_get(
      ob, paint_mode, brush_paint, scene->toolsettings->imapaint.paint.visible_material_channels);
  for (const PaintMaterialImageTarget &material_target : material_targets) {
    if (material_target.image == nullptr || !ID_IS_EDITABLE(material_target.image) ||
        ID_IS_OVERRIDE_LIBRARY(material_target.image))
    {
      continue;
    }
    if (std::any_of(targets.begin(), targets.end(), [&](const ImagePaintSelectionTarget &target) {
          return target.image == material_target.image;
        }))
    {
      continue;
    }
    targets.append({material_target.image,
                    material_target.iuser ? *material_target.iuser : canvas_iuser,
                    material_target.is_color_channel,
                    material_target.channel});
  }
  /* A temporarily unresolved Material canvas keeps the single-canvas behavior. */
  if (targets.is_empty() && canvas_image != nullptr) {
    targets.append({canvas_image, canvas_iuser});
  }
  return targets;
}

bool image_paint_selection_view3d_primary_target_get(bContext *C,
                                                     Object &ob,
                                                     ImagePaintSelectionTarget &r_target)
{
  /* The masks of all targets are identical by construction, so the first resolved target is the
   * one to display; it must come from the same resolver the tools write through. */
  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C,
                                                                                            ob);
  if (targets.is_empty()) {
    return false;
  }
  r_target = targets.first();
  return true;
}

void image_paint_selection_view3d_undo_begin(const char *name,
                                             const Span<ImagePaintSelectionTarget> targets)
{
  ED_image_undo_push_begin(name, PaintMode::Sculpt);
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ED_image_undo_capture_selection_mask(target.image, tile->tile_number);
    }
  }
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
  const bool rotate_normals = is_normal_map &&
                              !image_select_normal_rotation_is_identity(normal_rotation);

  /* Re-apply the cut the lift made originally: #image_select_fragment_commit_with_undo restored
   * the source region to its pre-lift original for the "before" snapshot. Idempotent. */
  image_select_fragment_lift_source(C, ima, base_iuser, fragments);

  /* The inverse of the caller's forward transform: source_uv = pivot + L^-1 * (dest - pivot - t).
   * The linear map is constant for the whole commit, so one inversion here serves every pixel. */
  bool invert_ok = false;
  const float2x2 linear_inv = math::invert(linear_uv, invert_ok);
  if (!invert_ok) {
    return;
  }

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

      /* The destination region is the fragment's full UV footprint mapped through the affine
       * transform, clipped to the tile: transform the four footprint corners and take their
       * bounds. */
      float2 dest_min(FLT_MAX, FLT_MAX);
      float2 dest_max(-FLT_MAX, -FLT_MAX);
      for (const int i : IndexRange(4)) {
        const float2 corner_src(frag_origin_uv + float2((i & 1) ? frag_size_uv.x : 0.0f,
                                                        (i & 2) ? frag_size_uv.y : 0.0f));
        const float2 corner_dst = pivot_uv +
                                  linear_uv * (corner_src - pivot_uv) + translation_uv;
        dest_min = math::min(dest_min, corner_dst);
        dest_max = math::max(dest_max, corner_dst);
      }
      const int dp_x0 = std::max(0, int(std::floor((dest_min.x - dst_tile_uv.x) * dst_w)));
      const int dp_y0 = std::max(0, int(std::floor((dest_min.y - dst_tile_uv.y) * dst_h)));
      const int dp_x1 = std::min(int(dst_w), int(std::ceil((dest_max.x - dst_tile_uv.x) * dst_w)));
      const int dp_y1 = std::min(int(dst_h), int(std::ceil((dest_max.y - dst_tile_uv.y) * dst_h)));
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
              const float2 src_uv = pivot_uv +
                                    linear_inv * (dest_uv - pivot_uv - translation_uv);
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
                image_select_normal_color_rotate(frag_color, normal_rotation);
              }
              const float frag_alpha = (channels >= 4) ? frag_color[3] : 1.0f;
              const float blend = m * frag_alpha;
              if (blend <= 0.001f) {
                continue;
              }
              float *dst_px = dst_data + (py * int(dst_w) + px) * channels;
              for (int c = 0; c < channels; c++) {
                dst_px[c] = (1.0f - blend) * dst_px[c] + blend * frag_color[c];
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
              const float2 src_uv = pivot_uv +
                                    linear_inv * (dest_uv - pivot_uv - translation_uv);
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
                image_select_normal_color_rotate(frag_color, normal_rotation);
              }
              const float frag_alpha = frag_color[3];
              const float blend = m * frag_alpha;
              if (blend <= 0.001f) {
                continue;
              }
              uint8_t *dst_px = dst_data + (py * int(dst_w) + px) * 4;
              for (int c = 0; c < 4; c++) {
                /* Round rather than truncate (truncation biases the feathered rim down and
                 * compounds across repeated edits; matches the 2D write-back). */
                dst_px[c] = uint8_t(
                    std::clamp((1.0f - blend) * float(dst_px[c]) +
                                   blend * frag_color[c] * 255.0f + 0.5f,
                               0.0f,
                               255.0f));
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
                    const float2 src_uv = pivot_uv +
                                          linear_inv * (dest_uv - pivot_uv - translation_uv);
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Anchor and plane mapping
 * \{ */

bool ImageSelectView3DAnchor::project_screen(const float3 &co_object, float2 &r_px) const
{
  const float4 clip = this->screen->projmat * float4(co_object.x, co_object.y, co_object.z, 1.0f);
  if (clip.w <= 1e-6f) {
    return false;
  }
  r_px = (float2(clip.x, clip.y) / clip.w * 0.5f + float2(0.5f)) * this->screen->region_size;
  return true;
}

bool ImageSelectView3DAnchor::project_object_point(const float3 &co_object,
                                                   const float3 &no_object,
                                                   float2 &r_px) const
{
  if (this->screen) {
    /* Front faces only, like the Sculpt Mode gestures' "Front Faces Only". */
    const float3 to_view = this->screen->is_persp ? this->screen->view_origin - co_object :
                                                    -this->screen->view_forward;
    if (math::dot(no_object, to_view) <= 0.0f) {
      return false;
    }
    return this->project_screen(co_object, r_px);
  }
  if (!shape::surface_anchor_hit_accepts(
          anchor.co, frame.normal, co_object, no_object, max_depth))
  {
    return false;
  }
  const float3 d = co_object - anchor.co;
  r_px = float2(math::dot(d, frame.tangent), math::dot(d, frame.bitangent)) * anchor.px_per_unit;
  return true;
}

float3 ImageSelectView3DAnchor::plane_px_to_object(const float2 &px) const
{
  const float2 units = px / std::max(anchor.px_per_unit, 1e-6f);
  return anchor.co + frame.tangent * units.x + frame.bitangent * units.y;
}

/** Solve the affine plane-px -> UV mapping of a triangle: `uv = uv0 + J * (p - p0)`. */
static bool plane_uv_jacobian_from_tri(const float2 &p0,
                                       const float2 &p1,
                                       const float2 &p2,
                                       const float2 &uv0,
                                       const float2 &uv1,
                                       const float2 &uv2,
                                       float2x2 &r_jacobian)
{
  float2x2 edges;
  edges[0] = p1 - p0;
  edges[1] = p2 - p0;
  float2x2 uv_edges;
  uv_edges[0] = uv1 - uv0;
  uv_edges[1] = uv2 - uv0;
  bool success = false;
  const float2x2 inverse = math::invert(edges, success);
  if (!success) {
    return false;
  }
  r_jacobian = uv_edges * inverse;
  return true;
}

bool image_select_view3d_anchor_from_mval(bContext *C,
                                          const int2 &mval,
                                          ImageSelectView3DAnchor &r_anchor,
                                          const bool use_screen_space)
{
  Object *ob = CTX_data_active_object(C);
  const ARegion *region = CTX_wm_region(C);
  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const Scene *scene = CTX_data_scene(C);
  const View3D *v3d = CTX_wm_view3d(C);
  if (ob == nullptr || region == nullptr || rv3d == nullptr || depsgraph == nullptr ||
      scene == nullptr || v3d == nullptr || scene->toolsettings == nullptr)
  {
    return false;
  }
  const Sculpt *sculpt = scene->toolsettings->sculpt;
  if (sculpt == nullptr) {
    return false;
  }

  r_anchor = ImageSelectView3DAnchor{};
  r_anchor.object = ob;

  if (use_screen_space) {
    /* View mode needs no surface under the cursor: the shape is screen-space, so the gesture may
     * start off the mesh. */
    const float4x4 world_to_object_view = math::invert(float4x4(ob->object_to_world()));
    ImageSelectView3DAnchor::ScreenSpace screen;
    screen.projmat = ED_view3d_ob_project_mat_get(rv3d, ob);
    screen.region_size = float2(float(region->winx), float(region->winy));
    screen.is_persp = rv3d->is_persp;
    screen.view_origin = math::transform_point(
        world_to_object_view, float3(rv3d->viewinv[3][0], rv3d->viewinv[3][1], rv3d->viewinv[3][2]));
    screen.view_forward = math::normalize(math::transform_direction(
        world_to_object_view,
        float3(-rv3d->viewinv[2][0], -rv3d->viewinv[2][1], -rv3d->viewinv[2][2])));
    /* Occlusion, like the paint shapes' View projection: the select ID buffer tells which faces
     * are actually seen, so a gesture does not reach through nearer geometry. Left empty (every
     * face passes) in X-Ray, which selects through the surface. */
    if (!XRAY_ENABLED(v3d)) {
      const Main *bmain = CTX_data_main(C);
      ViewLayer *view_layer = CTX_data_view_layer(C);
      if (bmain != nullptr && view_layer != nullptr) {
        BKE_view_layer_synced_ensure(*bmain, scene, view_layer);
        if (Base *base = BKE_view_layer_base_find(view_layer, ob)) {
          Vector<Base *> bases = {base};
          DRW_select_buffer_context_create(depsgraph, bases, SCE_SELECT_FACE);
          rcti region_rect;
          BLI_rcti_init(&region_rect, 0, region->winx, 0, region->winy);
          uint bitmap_num = 0;
          BLI_bitmap *bitmap = DRW_select_buffer_bitmap_from_rect(
              depsgraph,
              const_cast<ARegion *>(region),
              const_cast<View3D *>(v3d),
              &region_rect,
              &bitmap_num);
          if (bitmap != nullptr) {
            const uint offset = DRW_select_buffer_context_offset_for_object_elem(
                depsgraph, ob, SCE_SELECT_FACE);
            const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
            Array<bool> face_visible(mesh.faces_num, false);
            int visible_num = 0;
            for (const int face : IndexRange(mesh.faces_num)) {
              const uint index = offset + uint(face);
              face_visible[face] = index < bitmap_num && BLI_BITMAP_TEST_BOOL(bitmap, index);
              visible_num += int(face_visible[face]);
            }
            MEM_SAFE_DELETE(bitmap);
            /* An ID buffer that saw no face of this object is not a usable occlusion answer (the
             * buffer was not drawn for it), so fail open rather than selecting nothing, ever. */
            if (visible_num > 0) {
              screen.face_visible = std::move(face_visible);
            }
          }
        }
      }
    }
    r_anchor.screen = screen;
    return true;
  }

  /* Same raycast as the 3D paint-shape invoke. resolve_hit_object = false: the selection anchors
   * to the operator's active object, and the cursor side effects must not switch it. */
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
  const std::optional<CursorGeometryInfo> hit = cursor_geometry_info_update(*depsgraph,
                                                                            sculpt->paint,
                                                                            sculpt,
                                                                            vc,
                                                                            CTX_data_active_base(C),
                                                                            float2(mval),
                                                                            false,
                                                                            false,
                                                                            nullptr);
  if (!hit) {
    return false;
  }

  const float4x4 object_to_world(ob->object_to_world());
  const float4x4 world_to_object = math::invert(object_to_world);
  const float3 anchor_object = hit->location;
  const float3 normal_object = math::normalize(hit->normal);

  /* Tangent = screen horizontal in object space; the shared frame orthonormalizes it, so the
   * authored plane-px use the same basis the space projection and the overlay use. */
  const float3 view_x_world = float3(
      rv3d->viewinv[0][0], rv3d->viewinv[0][1], rv3d->viewinv[0][2]);
  r_anchor.frame = shape::surface_anchor_frame(
      normal_object, math::transform_direction(world_to_object, view_x_world));
  r_anchor.anchor.co = anchor_object;
  r_anchor.anchor.normal = normal_object;
  r_anchor.anchor.tangent = r_anchor.frame.tangent;
  r_anchor.anchor.max_depth = 0.0f;

  const float3 anchor_world = math::transform_point(object_to_world, anchor_object);
  const float anchor_world_v[3] = {anchor_world.x, anchor_world.y, anchor_world.z};
  const float pixel_size_world = ED_view3d_pixel_size(rv3d, anchor_world_v);
  const float px_per_unit_world = 1.0f / std::max(pixel_size_world, 1e-6f);
  r_anchor.anchor.px_per_unit = shape::surface_anchor_px_per_unit(
      px_per_unit_world, object_to_world, r_anchor.frame.tangent, r_anchor.frame.bitangent);

  /* UV / triangle restoration cache plus the plane-px -> UV Jacobian of the hit triangle. */
  const SculptSession *ss = ob->runtime ? ob->runtime->sculpt_session : nullptr;
  const int hit_face = (ss != nullptr) ? ss->active_face_index.value_or(-1) : -1;
  if (!shape::surface_anchor_cache_fill(*ob, *depsgraph, hit_face, r_anchor.anchor)) {
    return true;
  }
  if (!r_anchor.anchor.has_surface_uv) {
    return true;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  const StringRefNull uv_name = r_anchor.anchor.surface_uv_map[0] != '\0' ?
                                    StringRefNull(r_anchor.anchor.surface_uv_map) :
                                    StringRefNull(mesh.active_or_default_uv_map_name());
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      uv_name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return true;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob);
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_verts = mesh.corner_verts();
  if (r_anchor.anchor.tri < 0 || r_anchor.anchor.tri >= corner_tris.size()) {
    return true;
  }
  const int3 tri = corner_tris[r_anchor.anchor.tri];
  float2 tri_px[3];
  for (const int i : IndexRange(3)) {
    const float3 corner_co = positions[corner_verts[tri[i]]];
    const float3 d = corner_co - anchor_object;
    tri_px[i] = float2(math::dot(d, r_anchor.frame.tangent),
                       math::dot(d, r_anchor.frame.bitangent)) *
                r_anchor.anchor.px_per_unit;
  }
  r_anchor.jacobian_valid = plane_uv_jacobian_from_tri(tri_px[0],
                                                       tri_px[1],
                                                       tri_px[2],
                                                       uv_map[tri[0]],
                                                       uv_map[tri[1]],
                                                       uv_map[tri[2]],
                                                       r_anchor.plane_uv_jacobian);
  r_anchor.anchor_uv = r_anchor.anchor.surface_uv;
  return true;
}

bool image_select_view3d_mval_to_plane_px(const ImageSelectView3DAnchor &anchor,
                                          const ARegion &region,
                                          const int2 &mval,
                                          float2 &r_px)
{
  if (anchor.screen) {
    r_px = float2(mval);
    return true;
  }
  const Object *ob = anchor.object;
  if (ob == nullptr) {
    return false;
  }
  const float4x4 object_to_world(ob->object_to_world());
  const float4x4 world_to_object = math::invert(object_to_world);
  /* World plane through the anchor, normal from the inverse-transpose (non-uniform scale safe). */
  const float3 normal_world = math::normalize(
      math::transform_direction(math::transpose(world_to_object), anchor.frame.normal));
  const float3 anchor_world = math::transform_point(object_to_world, anchor.anchor.co);
  const float plane[4] = {normal_world.x,
                          normal_world.y,
                          normal_world.z,
                          -math::dot(normal_world, anchor_world)};
  const float mval_f[2] = {float(mval.x), float(mval.y)};
  float world[3];
  if (!ED_view3d_win_to_3d_on_plane(&region, plane, mval_f, false, world)) {
    /* Ray (near-)parallel to the plane: the caller keeps its last valid point. */
    return false;
  }
  const float3 co_object = math::transform_point(world_to_object,
                                                 float3(world[0], world[1], world[2]));
  const float3 d = co_object - anchor.anchor.co;
  r_px = float2(math::dot(d, anchor.frame.tangent), math::dot(d, anchor.frame.bitangent)) *
         anchor.anchor.px_per_unit;
  return true;
}

float2 image_select_view3d_plane_px_to_uv_delta(const ImageSelectView3DAnchor &anchor,
                                                const float2 &plane_px_delta)
{
  if (!anchor.jacobian_valid) {
    return float2(0.0f);
  }
  return anchor.plane_uv_jacobian * plane_px_delta;
}

bool image_select_view3d_plane_px_to_surface_uv(bContext *C,
                                                const ImageSelectView3DAnchor &anchor,
                                                const float2 &plane_px,
                                                float2 &r_uv)
{
  if (anchor.object == nullptr) {
    return false;
  }
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return false;
  }
  const float3 co_object = anchor.plane_px_to_object(plane_px);
  shape::SurfaceAnchor probe = anchor.anchor;
  probe.co = co_object;
  probe.tri = -1;
  probe.tri_corners = int3(-1);
  if (!shape::surface_anchor_cache_fill(*anchor.object, *depsgraph, -1, probe)) {
    return false;
  }
  if (!probe.has_surface_uv) {
    return false;
  }
  r_uv = probe.surface_uv;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask rasterization through the mesh
 * \{ */

/**
 * Copy the rasterized selection masks of \a source into \a dest. Both images belong to targets of
 * the same mesh, so tile numbers and mask contents correspond 1:1; tiles whose resolutions differ
 * (e.g. PBR maps of different sizes) are resampled instead of copied. Skips tiles whose buffer is
 * missing on \a dest, so masks are never created where nothing can be painted.
 */
static void rasterized_masks_copy(const Image &source,
                                  Image &dest,
                                  const ImageUser &dest_iuser)
{
  for (const ImageTile *tile : ConstListBaseWrapper<ImageTile>(source.tiles)) {
    const ImBuf *src = BKE_image_paint_selection_mask_lookup(&source, tile->tile_number);
    if (src == nullptr || src->float_data() == nullptr) {
      continue;
    }
    /* Resolve the destination tile's size: from its existing mask, or from the tile buffer when
     * the mask does not exist yet. */
    int dst_w = 0;
    int dst_h = 0;
    const ImBuf *dst_existing = BKE_image_paint_selection_mask_lookup(&dest, tile->tile_number);
    if (dst_existing != nullptr && dst_existing->float_data() != nullptr) {
      dst_w = dst_existing->x;
      dst_h = dst_existing->y;
    }
    else {
      ImBuf *ibuf = image_select_v3d::tile_ibuf_get(&dest, dest_iuser, tile->tile_number);
      if (ibuf == nullptr) {
        continue;
      }
      dst_w = ibuf->x;
      dst_h = ibuf->y;
    }
    /* Same size as the existing mask keeps it (and bumps the revision); a size change reallocates
     * it. Never pass the source size here: the mask must keep matching its own image. */
    ImBuf *dst = BKE_image_paint_selection_mask_get(&dest, tile->tile_number, dst_w, dst_h);
    if (dst == nullptr || dst->float_data_for_write() == nullptr) {
      continue;
    }
    const float *src_data = src->float_data();
    float *dst_data = dst->float_data_for_write();
    if (src->x == dst->x && src->y == dst->y) {
      memcpy(dst_data, src_data, sizeof(float) * size_t(src->x) * size_t(src->y));
      continue;
    }
    /* Nearest resample: the masks are binary (feathering derives from them later), so a plain
     * point lookup preserves the boundary better than a smoothed filter. */
    for (const int y : IndexRange(dst->y)) {
      const int sy = std::clamp(int(int64_t(y) * src->y / dst->y), 0, src->y - 1);
      for (const int x : IndexRange(dst->x)) {
        const int sx = std::clamp(int(int64_t(x) * src->x / dst->x), 0, src->x - 1);
        dst_data[int64_t(y) * dst->x + x] = src_data[int64_t(sy) * src->x + sx];
      }
    }
  }
}

bool image_select_view3d_rasterize_masks(bContext *C,
                                         Object &ob,
                                         const Depsgraph &depsgraph,
                                         const Span<ImagePaintSelectionTarget> targets,
                                         const ImageSelectView3DAnchor &anchor,
                                         const rctf &domain_px,
                                         const FunctionRef<bool(const float2 &plane_px)> inside_fn,
                                         const float fill_value,
                                         Vector<int> *r_touched_faces)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return false;
  }
  if (targets.is_empty() || domain_px.xmin > domain_px.xmax || domain_px.ymin > domain_px.ymax) {
    return false;
  }

  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(depsgraph, ob);
  const MeshAttributeData attribute_data(mesh);
  FaceSelectionMask face_selection_mask;
  face_selection_mask_build(mesh, face_selection_mask);
  const bool use_face_selection = face_selection_mask.state == FaceSelectionState::Active;
  const Span<int> corner_tri_faces = mesh.corner_tri_faces();
  const Span<bool> hide_poly = attribute_data.hide_poly;

  Vector<ePaintSymmetryFlags> passes;
  const int symmetry_flags = int(mesh_symmetry_xyz_get(ob));
  for (int i = 0; i <= symmetry_flags; i++) {
    if (is_symmetry_iteration_valid(i, symmetry_flags)) {
      passes.append(ePaintSymmetryFlags(i));
    }
  }

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  Vector<int> candidates;
  for (const int i : nodes.index_range()) {
    const Bounds<float3> &bounds = nodes[i].bounds();
    rctf node_rct;
    BLI_rctf_init(&node_rct, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
    bool any = false;
    for (int corner = 0; corner < 8; corner++) {
      const float3 p = image_select_v3d::bounds_corner(bounds, corner);
      for (const ePaintSymmetryFlags pass : passes) {
        float2 co;
        if (!image_select_v3d::project_raw(anchor, symmetry_flip(p, pass), co)) {
          /* Behind the view: the node's extent is unbounded, never cull it. */
          BLI_rctf_init(&node_rct, -FLT_MAX, FLT_MAX, -FLT_MAX, FLT_MAX);
          any = true;
          break;
        }
        node_rct.xmin = std::min(node_rct.xmin, co.x);
        node_rct.xmax = std::max(node_rct.xmax, co.x);
        node_rct.ymin = std::min(node_rct.ymin, co.y);
        node_rct.ymax = std::max(node_rct.ymax, co.y);
        any = true;
      }
    }
    rctf isect;
    if (any && BLI_rctf_isect(&node_rct, &domain_px, &isect)) {
      candidates.append(i);
    }
  }
  if (candidates.is_empty()) {
    return false;
  }
  IndexMaskMemory memory;
  const IndexMask node_mask = IndexMask::from_indices(candidates.as_span(), memory);

  Scene *scene = CTX_data_scene(C);
  const StringRef uv_map_name = BKE_paint_canvas_uvmap_name_get(&scene->toolsettings->paint_mode, &ob)
                                    .value_or("");

  std::atomic<bool> any_written{false};

  /* A local mutable copy: #ImageData::from_image and #build_pixels need a mutable ImageUser,
   * and the caller passes a const span. */
  Vector<ImagePaintSelectionTarget> targets_mutable(targets.begin(), targets.end());
  threading::EnumerableThreadSpecific<image_select_v3d::RasterTLS> all_tls;
  /* The first target whose buffers resolve: its masks are rasterized geometrically, the others
   * get exact copies (see #rasterized_masks_copy). */
  Image *source_image = nullptr;
  for (const int target_i : targets_mutable.index_range()) {
    ImagePaintSelectionTarget &target = targets_mutable[target_i];
    Image *image = target.image;
    if (source_image != nullptr) {
      /* Copying avoids re-running the geometric rasterization and #build_pixels (which rebuilds
       * the PBVH pixel data and invalidates the paint cache once per image) for every Material
       * target; the masks are identical by construction. */
      rasterized_masks_copy(*source_image, *image, target.iuser);
      continue;
    }
    std::unique_ptr<paint::image::ImageData> image_data = paint::image::ImageData::from_image(
        image, &target.iuser);
    if (!image_data) {
      continue;
    }
    if (!bke::pbvh::build_pixels(depsgraph, ob, *image, target.iuser, uv_map_name)) {
      continue;
    }
    bke::pbvh::pixels::PixelData &pixel_data = bke::pbvh::pixels::data_get(*pbvh);
    MutableSpan<bke::pbvh::pixels::PixelNode> pixel_nodes = pixel_data.nodes;

    /* Sequential: #fetch_image_buffers inserts into non-thread-safe maps, and every writable
     * mask tile is created once here (which also advances the mask revision) so the parallel
     * pass below only writes pixels through the captured pointers. */
    Map<bke::image::TileNumber, ImBuf *> mask_ptrs;
    node_mask.foreach_index([&](const int i) {
      bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
      if (pixel_node.tiles.is_empty()) {
        return;
      }
      paint::image::fetch_image_buffers(*image_data, nodes[i], pixel_node);
      for (const bke::pbvh::pixels::UDIMTilePixels &tile : pixel_node.tiles) {
        const ImBuf *ibuf = image_data->buffers.lookup_default(tile.tile_number, nullptr);
        if (ibuf == nullptr) {
          continue;
        }
        ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile.tile_number, ibuf->x, ibuf->y);
        if (mask != nullptr) {
          mask_ptrs.add(tile.tile_number, mask);
        }
      }
    });

    node_mask.foreach_index(
        [&](const int i) {
          bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
          if (pixel_node.tiles.is_empty()) {
            return;
          }
          image_select_v3d::RasterTLS &tls = all_tls.local();
          for (bke::pbvh::pixels::UDIMTilePixels &tile : pixel_node.tiles) {
            const ImBuf *ibuf = image_data->buffers.lookup_default(tile.tile_number, nullptr);
            if (ibuf == nullptr) {
              continue;
            }
            /* Writable pointer captured in the sequential phase above (no revision bump per
             * node, which a fresh mutable lookup would cause inside this parallel pass). */
            ImBuf *mask = mask_ptrs.lookup_default(tile.tile_number, nullptr);
            if (mask == nullptr || mask->x != ibuf->x || mask->y != ibuf->y) {
              continue;
            }
            float *mask_px = mask->float_data_for_write();

            threading::parallel_for(
                tile.pixel_rows.index_range(), 64, [&](const IndexRange rows) {
                  Vector<float3> positions;
                  for (const int r : rows) {
                    const bke::pbvh::pixels::PackedPixelRow &pixel_row = tile.pixel_rows[r];
                    const int tri_local = pixel_row.uv_primitive_index;
                    if (tri_local < 0 ||
                        tri_local >= pixel_node.uv_primitives.tri_indices.size())
                    {
                      continue;
                    }
                    const int tri = pixel_node.uv_primitives.tri_indices[tri_local];
                    if (tri < 0 || tri >= pixel_data.vert_tris.size() ||
                        tri >= corner_tri_faces.size())
                    {
                      continue;
                    }
                    const int face = corner_tri_faces[tri];
                    if (!hide_poly.is_empty() && hide_poly[face]) {
                      continue;
                    }
                    if (use_face_selection && !face_selection_mask.select_poly[face]) {
                      continue;
                    }
                    if (anchor.screen && !anchor.screen->face_visible.is_empty() &&
                        !anchor.screen->face_visible[face])
                    {
                      continue;
                    }
                    const int3 tri_verts = pixel_data.vert_tris[tri];
                    if (tri_verts.x >= vert_positions.size() ||
                        tri_verts.y >= vert_positions.size() ||
                        tri_verts.z >= vert_positions.size())
                    {
                      continue;
                    }
                    const float3 v0 = vert_positions[tri_verts.x];
                    const float3 v1 = vert_positions[tri_verts.y];
                    const float3 v2 = vert_positions[tri_verts.z];
                    const float3 tri_normal = math::normalize(math::cross(v1 - v0, v2 - v0));

                    const IndexRange range(0, pixel_row.num_pixels);
                    positions.resize(range.size());
                    paint::image::calc_pixel_row_positions(vert_positions,
                                                           pixel_data.vert_tris,
                                                           pixel_node.uv_primitives.tri_indices,
                                                           pixel_node.uv_primitives.delta_barycentric_coords,
                                                           pixel_row,
                                                           range,
                                                           positions);

                    bool changed = false;
                    for (const int px : range.index_range()) {
                      const float3 position = positions[px];
                      for (const ePaintSymmetryFlags pass : passes) {
                        const float3 mirrored = symmetry_flip(position, pass);
                        const float3 mirrored_normal = symmetry_flip(tri_normal, pass);
                        float2 co;
                        if (!anchor.project_object_point(mirrored, mirrored_normal, co)) {
                          continue;
                        }
                        if (!inside_fn(co)) {
                          continue;
                        }
                        const int x = int(pixel_row.start_image_coordinate.x) + px;
                        const int y = int(pixel_row.start_image_coordinate.y);
                        if (x < 0 || y < 0 || x >= mask->x || y >= mask->y) {
                          continue;
                        }
                        mask_px[int64_t(y) * mask->x + x] = fill_value;
                        changed = true;
                        break;
                      }
                    }
                    if (changed) {
                      if (r_touched_faces != nullptr) {
                        tls.touched_faces.append(face);
                      }
                      any_written.store(true, std::memory_order_relaxed);
                    }
                  }
                });
          }
        },
        exec_mode::grain_size(1));

    source_image = image;
  }

  if (r_touched_faces != nullptr) {
    /* Collect touched faces from all threads. Each face may be added multiple times
     * (once per pixel row), so deduplicate using a Set. */
    Set<int> touched_faces_set;
    for (image_select_v3d::RasterTLS &tls : all_tls) {
      for (const int face : tls.touched_faces) {
        touched_faces_set.add(face);
      }
    }
    for (const int face : touched_faces_set) {
      r_touched_faces->append(face);
    }
  }

  return any_written.load();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FACE / ISLAND expansion
 * \{ */

namespace image_select_v3d {

/** Rasterize \a faces' full UV footprints (per corner triangle) into every tile mask of \a
 * targets the footprints reach. Pure UV-space work on the original mesh, like the 2D expansion. */
void expand_faces_to_masks(const bContext *C,
                           const Object &ob,
                           const Span<ImagePaintSelectionTarget> targets,
                           const Span<int> faces,
                           const float fill_value)
{
  if (faces.is_empty() || targets.is_empty()) {
    return;
  }
  const Scene *scene = CTX_data_scene(C);
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  const StringRef uv_name = BKE_paint_canvas_uvmap_name_get(
                                &scene->toolsettings->paint_mode, const_cast<Object *>(&ob))
                                .value_or(mesh.active_or_default_uv_map_name());
  const bke::AttributeReader<float2> uv_attribute = mesh.attributes().lookup<float2>(
      uv_name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  const Span<int3> corner_tris = mesh.corner_tris();
  const Span<int> corner_tri_faces = mesh.corner_tri_faces();
  const OffsetIndices<int> faces_offsets = mesh.faces();

  for (const int face : faces) {
    if (face < 0 || face >= mesh.faces_num) {
      continue;
    }
    /* UV bounds of the face across its corner triangles. */
    rctf face_uv;
    BLI_rctf_init(&face_uv, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
    for (const int tri_i : bke::mesh::face_triangles_range(faces_offsets, face)) {
      const int3 tri = corner_tris[tri_i];
      for (const int i : IndexRange(3)) {
        const float2 uv = uv_map[tri[i]];
        const float xy[2] = {uv.x, uv.y};
        BLI_rctf_do_minmax_v(&face_uv, xy);
      }
    }
    BLI_assert(face_uv.xmin <= face_uv.xmax);

    for (const ImagePaintSelectionTarget &target : targets) {
      Image *image = target.image;
      for (const ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
        const float2 tile_origin = BKE_image_get_tile_uv_origin(tile->tile_number);
        rctf tile_uv;
        tile_uv.xmin = tile_origin.x;
        tile_uv.ymin = tile_origin.y;
        tile_uv.xmax = tile_origin.x + 1.0f;
        tile_uv.ymax = tile_origin.y + 1.0f;
        rctf isect;
        if (!BLI_rctf_isect(&face_uv, &tile_uv, &isect)) {
          continue;
        }
        ImBuf *ibuf = tile_ibuf_get(image, target.iuser, tile->tile_number);
        if (ibuf == nullptr) {
          continue;
        }
        ImBuf *mask = BKE_image_paint_selection_mask_get(
            image, tile->tile_number, ibuf->x, ibuf->y);
        float *mask_px = mask->float_data_for_write();
        for (const int tri_i : bke::mesh::face_triangles_range(faces_offsets, face)) {
          const int3 tri = corner_tris[tri_i];
          float2 px_verts[3];
          for (const int i : IndexRange(3)) {
            px_verts[i] = (uv_map[tri[i]] - tile_origin) * float2(float(mask->x), float(mask->y));
          }
          foreach_uv_polygon_pixel(
              Span<float2>(px_verts, 3), mask->x, mask->y, [&](const int x, const int y, bool) {
                mask_px[int64_t(y) * mask->x + x] = fill_value;
                return true;
              });
        }
      }
    }
  }
}

/** UV-island expansion: flood-fill the islands containing \a seed_faces in the original mesh's
 * UV layout (a temporary BMesh, like the 2D expansion) and rasterize them. */
void expand_islands_to_masks(const bContext *C,
                             Object &ob,
                             const Span<ImagePaintSelectionTarget> targets,
                             const Span<int> seed_faces,
                             const float fill_value)
{
  if (seed_faces.is_empty()) {
    return;
  }
  Scene *scene = CTX_data_scene(C);
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr) {
    return;
  }
  const BMAllocTemplate allocsize = BMALLOC_TEMPLATE_FROM_ME(mesh);
  BMeshCreateParams create_params{};
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = true;
  convert_params.calc_vert_normal = true;
  BMesh *bm = BM_mesh_create(&allocsize, &create_params);
  BM_mesh_bm_from_me(bm, mesh, &convert_params);

  const BMUVOffsets offsets = image_paint_selection_uv_offsets_get(bm, &ob, scene);
  if (offsets.uv >= 0) {
    Array<bool> island_face_tag(bm->totface, false);
    ED_uvedit_uv_islands_tag_from_face_indices(
        scene, bm, offsets, seed_faces, 0, island_face_tag);
    Vector<int> island_faces;
    for (const int i : island_face_tag.index_range()) {
      if (island_face_tag[i]) {
        island_faces.append(i);
      }
    }
    expand_faces_to_masks(C, ob, targets, island_faces, fill_value);
  }
  BM_mesh_free(bm);
}

}  // namespace image_select_v3d

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared gesture apply
 * \{ */

bool image_select_view3d_gesture_apply(bContext *C,
                                       Object &ob,
                                       const ImageSelectView3DAnchor &anchor_in,
                                       const rctf &domain_px,
                                       const FunctionRef<bool(const float2 &plane_px)> inside_fn,
                                       const PaintSelectionEdgePolicy edge_policy,
                                       const char *undo_name,
                                       const eSelectOp sel_op)
{
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, ob);
  if (targets.is_empty()) {
    return false;
  }
  if (domain_px.xmin > domain_px.xmax || domain_px.ymin > domain_px.ymax) {
    return false;
  }
  /* The expansion level is the shared tool setting (drawn in the tool header), not an operator
   * property. */
  const int expand = CTX_data_scene(C) != nullptr ?
                         CTX_data_scene(C)->toolsettings->imapaint.selection_expand :
                         IMAGE_PAINT_SELECT_EXPAND_PIXELS;

  /* Depth rejection: half the gesture's larger half-axis, in object units, with a floor -- the
   * same rule the 3D paint shapes use for their anchor. */
  ImageSelectView3DAnchor anchor = anchor_in;
  const float2 size_px(domain_px.xmax - domain_px.xmin, domain_px.ymax - domain_px.ymin);
  const float half_object = 0.5f * math::length(size_px) /
                            std::max(anchor.anchor.px_per_unit, 1e-6f);
  anchor.max_depth = std::max(SELECT_VIEW3D_MIN_DEPTH,
                              SELECT_VIEW3D_MAX_DEPTH_FACTOR * half_object);
  anchor.anchor.max_depth = anchor.max_depth;

  const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr) {
    return false;
  }

  image_paint_selection_view3d_undo_begin(undo_name, targets);
  if (sel_op == SEL_OP_SET) {
    for (const ImagePaintSelectionTarget &target : targets) {
      BKE_image_paint_selection_mask_free(target.image);
    }
  }

  Vector<int> touched_faces;
  image_select_view3d_rasterize_masks(
      C, ob, *depsgraph, targets, anchor, domain_px, inside_fn, fill_value, &touched_faces);

  /* FACE expansion replaces the touched faces' footprints with full faces; ISLAND expansion
   * floods the touched faces' UV islands (2D parity: ISLAND does not grow a NEW selection). */
  if (expand == IMAGE_PAINT_SELECT_EXPAND_ISLAND && sel_op != SEL_OP_SET) {
    image_select_v3d::expand_islands_to_masks(C, ob, targets, touched_faces, fill_value);
  }
  else if (expand == IMAGE_PAINT_SELECT_EXPAND_FACE) {
    image_select_v3d::expand_faces_to_masks(C, ob, targets, touched_faces, fill_value);
  }

  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_edge_policy_set(target.image, edge_policy);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return true;
}

/** Simple click on a selection gesture: deselect everything (2D parity). */
static void view3d_apply_deselect(bContext *C, Object &ob)
{
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, ob);
  bool any = false;
  for (const ImagePaintSelectionTarget &target : targets) {
    any = any || BKE_image_paint_selection_mask_has_any(target.image);
  }
  if (!any) {
    return;
  }
  image_paint_selection_view3d_undo_begin("Deselect", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_mask_free(target.image);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name All / None / Invert operators
 * \{ */

static wmOperatorStatus view3d_select_all_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, *ob);
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_view3d_undo_begin("Select All", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      ImBuf *ibuf = image_select_v3d::tile_ibuf_get(
          target.image, target.iuser, tile->tile_number);
      if (ibuf == nullptr) {
        continue;
      }
      BKE_image_paint_selection_mask_fill(target.image, tile->tile_number, 1.0f);
    }
  }
  const PaintSelectionEdgePolicy hard = BKE_image_paint_selection_edge_policy_hard();
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_edge_policy_set(target.image, hard);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static wmOperatorStatus view3d_select_none_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  /* Bound to a plain click of the selection tools: a floating fragment is committed first, as the
   * gestures do, so the click never leaves a stale session behind. */
  if (image_select_view3d_session_active() != nullptr) {
    image_select_view3d_session_end(C, true);
  }
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, *ob);
  bool any = false;
  for (const ImagePaintSelectionTarget &target : targets) {
    any = any || BKE_image_paint_selection_mask_has_any(target.image);
  }
  if (!any) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_view3d_undo_begin("Deselect", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_mask_free(target.image);
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static wmOperatorStatus view3d_select_invert_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_view3d_targets_get(C, *ob);
  if (targets.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_view3d_undo_begin("Invert", targets);
  for (const ImagePaintSelectionTarget &target : targets) {
    for (const ImageTile *tile : ListBaseWrapper<ImageTile>(target.image->tiles)) {
      if (BKE_image_paint_selection_mask_lookup(target.image, tile->tile_number) != nullptr) {
        BKE_image_paint_selection_mask_invert(target.image, tile->tile_number);
      }
    }
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static void view3d_select_basic_props(wmOperatorType *ot)
{
  ot->flag = OPTYPE_REGISTER;
  ot->poll = image_paint_selection_view3d_poll;
}

void PAINT_OT_image_select_view3d_all(wmOperatorType *ot)
{
  ot->name = "All";
  ot->idname = "PAINT_OT_image_select_view3d_all";
  ot->description = "Select everything on the paint canvas";
  ot->exec = view3d_select_all_exec;
  view3d_select_basic_props(ot);
}

void PAINT_OT_image_select_view3d_none(wmOperatorType *ot)
{
  ot->name = "None";
  ot->idname = "PAINT_OT_image_select_view3d_none";
  ot->description = "Deselect everything on the paint canvas";
  ot->exec = view3d_select_none_exec;
  view3d_select_basic_props(ot);
  ot->poll = image_paint_selection_view3d_tool_poll;
}

void PAINT_OT_image_select_view3d_invert(wmOperatorType *ot)
{
  ot->name = "Invert";
  ot->idname = "PAINT_OT_image_select_view3d_invert";
  ot->description = "Invert the selection on the paint canvas";
  ot->exec = view3d_select_invert_exec;
  view3d_select_basic_props(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gesture operators (box / lasso / polyline / circle)
 *
 * Self-contained modals: the gesture geometry is accumulated in region coordinates and, on
 * confirm, mapped onto the anchor plane and rasterized through #image_select_view3d_gesture_apply.
 * Navigation input (orbit / zoom / pan) passes through to the default handlers, mirroring the 2D
 * polyline's discipline.
 * \{ */

enum class GestureKind {
  Box,
  Lasso,
  Polyline,
  Circle,
};

struct SelectGesture3DData {
  GestureKind kind = GestureKind::Box;
  ImageSelectView3DAnchor anchor;
  bool is_simple_click = true;
  int2 press_xy = int2(0);
  int2 last_xy = int2(0);
  /* Box. */
  bool box_valid = false;
  rctf box_region = {0.0f, 0.0f, 0.0f, 0.0f};
  /* Lasso / polyline. */
  Vector<float2> path_region;
  /* Circle. */
  float2 circle_center = float2(0.0f);
  float circle_radius = 0.0f;
  bool started = false;
  /** Space is held: the shape follows the cursor (see #gesture_move_handle). */
  bool move = false;
  /** Modifier keys (#KM_SHIFT / #KM_CTRL) seen since the gesture started. */
  int modifier_seen = 0;
  /* Preview draw callback (region px), registered in the invoke, removed in the finish. */
  ARegionType *owner_region_type = nullptr;
  void *draw_handle = nullptr;
};

/** True for a Box / Circle in Scene mode: a shape authored on the anchor plane (a rectangle /
 * circle of the surface, like the paint shapes' Surface projection), not a screen shape. */
static bool gesture_is_scene_shape(const SelectGesture3DData &data)
{
  return !data.anchor.screen && ELEM(data.kind, GestureKind::Box, GestureKind::Circle);
}

/** Region position of a plane-px point: the inverse of #image_select_view3d_mval_to_plane_px. */
static bool gesture_plane_px_to_region(const ImageSelectView3DAnchor &anchor,
                                       const ARegion &region,
                                       const float2 &plane_px,
                                       float2 &r_region)
{
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (anchor.object == nullptr || rv3d == nullptr) {
    return false;
  }
  const float3 world = math::transform_point(float4x4(anchor.object->object_to_world()),
                                             anchor.plane_px_to_object(plane_px));
  const float4 proj = float4x4(rv3d->persmat) * float4(world.x, world.y, world.z, 1.0f);
  if (proj.w <= 0.0f) {
    return false;
  }
  r_region = float2((proj.x / proj.w * 0.5f + 0.5f) * float(region.winx),
                    (proj.y / proj.w * 0.5f + 0.5f) * float(region.winy));
  return true;
}

/**
 * The plane-space shape of a Scene mode Box / Circle: the press point is its anchor, the cursor
 * the opposite corner (Box, axis-aligned in the tangent frame) or a point of the rim (Circle, the
 * radius is measured on the plane). \a r_a / \a r_b are the press and cursor in plane px.
 */
static bool gesture_scene_plane_points(const SelectGesture3DData &data,
                                       const ARegion &region,
                                       float2 &r_a,
                                       float2 &r_b)
{
  return image_select_view3d_mval_to_plane_px(data.anchor, region, data.press_xy, r_a) &&
         image_select_view3d_mval_to_plane_px(data.anchor, region, data.last_xy, r_b);
}

/** The plane-space outline of a Scene mode shape, closed, in plane px. */
static void gesture_scene_outline(const SelectGesture3DData &data,
                                  const float2 &a,
                                  const float2 &b,
                                  Vector<float2> &r_outline)
{
  if (data.kind == GestureKind::Box) {
    const float2 lo = math::min(a, b);
    const float2 hi = math::max(a, b);
    r_outline = {lo, float2(hi.x, lo.y), hi, float2(lo.x, hi.y)};
    return;
  }
  constexpr int SEGMENTS = 48;
  const float radius = math::distance(a, b);
  r_outline.clear();
  for (const int i : IndexRange(SEGMENTS)) {
    const float angle = (float(i) / float(SEGMENTS)) * 2.0f * float(M_PI);
    r_outline.append(a + float2(std::cos(angle), std::sin(angle)) * radius);
  }
}

/**
 * Region-post-pixel preview of the gesture the user is drawing. Without it the polyline is
 * unusable: its clicks are invisible until the mask lands, so there is no feedback where the
 * points land or that the tool even registered them.
 */
static void gesture_draw_preview(const bContext * /*C*/, ARegion *region, void *arg)
{
  auto *data = static_cast<SelectGesture3DData *>(arg);
  if (data == nullptr) {
    return;
  }
  /* Scene mode: the shape lives on the surface plane, so it is drawn there (projected back to the
   * region) and matches what the commit selects. */
  Vector<float2> scene_loop;
  if (gesture_is_scene_shape(*data) && region != nullptr && !data->is_simple_click) {
    float2 a, b;
    if (gesture_scene_plane_points(*data, *region, a, b)) {
      Vector<float2> outline;
      gesture_scene_outline(*data, a, b, outline);
      for (const float2 &p : outline) {
        float2 screen;
        if (gesture_plane_px_to_region(data->anchor, *region, p, screen)) {
          scene_loop.append(screen);
        }
      }
    }
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(1.0f);
  const uint pos = GPU_vertformat_attr_add(immVertexFormat(), "pos",
                                           gpu::VertAttrType::SFLOAT_32_32);
  /* No 2D uniform-color builtin in this version: the region-pixel overlays (like the 2D move
   * preview) bind the 3D one with a 2-component pos attribute. */
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.85f);

  if (gesture_is_scene_shape(*data)) {
    if (scene_loop.size() >= 3) {
      immBegin(GPU_PRIM_LINE_LOOP, uint(scene_loop.size()));
      for (const float2 &p : scene_loop) {
        immVertex2f(pos, p.x, p.y);
      }
      immEnd();
    }
  }
  else if (data->kind == GestureKind::Box && data->box_valid) {
    immBegin(GPU_PRIM_LINE_LOOP, 4);
    immVertex2f(pos, data->box_region.xmin, data->box_region.ymin);
    immVertex2f(pos, data->box_region.xmax, data->box_region.ymin);
    immVertex2f(pos, data->box_region.xmax, data->box_region.ymax);
    immVertex2f(pos, data->box_region.xmin, data->box_region.ymax);
    immEnd();
  }
  else if (ELEM(data->kind, GestureKind::Lasso, GestureKind::Polyline) &&
           !data->path_region.is_empty())
  {
    /* The polyline is click-built: draw the committed points plus the segment to the live cursor
     * position (#last_xy, refreshed on every MOUSEMOVE), so the click target is visible. */
    const int extra = (data->kind == GestureKind::Polyline) ? 1 : 0;
    immBegin(GPU_PRIM_LINE_STRIP, uint(data->path_region.size()) + extra);
    for (const float2 &p : data->path_region) {
      immVertex2f(pos, p.x, p.y);
    }
    if (extra != 0) {
      immVertex2f(pos, data->last_xy.x, data->last_xy.y);
    }
    immEnd();
  }
  else if (data->kind == GestureKind::Circle && data->circle_radius > 0.0f) {
    constexpr int SEGMENTS = 48;
    immBegin(GPU_PRIM_LINE_STRIP, SEGMENTS + 1);
    for (const int i : IndexRange(SEGMENTS + 1)) {
      const float angle = (float(i) / float(SEGMENTS)) * 2.0f * M_PI;
      immVertex2f(pos,
                  data->circle_center.x + std::cos(angle) * data->circle_radius,
                  data->circle_center.y + std::sin(angle) * data->circle_radius);
    }
    immEnd();
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);

  /* Angle of the segment being aimed, like the WM polyline gesture of the 2D editors. Hidden for
   * a trivial segment so a fresh click does not flicker a 0.0 degree readout. */
  if (data->kind == GestureKind::Polyline && !data->path_region.is_empty()) {
    const float2 anchor = data->path_region.last();
    const float2 live = float2(data->last_xy);
    if (math::distance_squared(live, anchor) > 1.0f) {
      const float2 delta = live - anchor;
      char text[64];
      SNPRINTF(text, "Angle = %.1f°", double(RAD2DEGF(std::atan2(delta.y, delta.x))));
      const float x = live.x + 15.0f * UI_SCALE_FAC;
      const float y = live.y + 20.0f * UI_SCALE_FAC;
      const int font_id = BLF_default();
      BLF_color4f(font_id, 0.0f, 0.0f, 0.0f, 0.5f);
      BLF_draw_default(x + 1.0f, y - 1.0f, 0.0f, text, strlen(text));
      BLF_color4f(font_id, 1.0f, 1.0f, 1.0f, 1.0f);
      BLF_draw_default(x, y, 0.0f, text, strlen(text));
    }
  }
}

/**
 * Holding Space while drawing moves the whole shape with the cursor instead of resizing it, like
 * the WM box / lasso gestures of the 2D editors (#GESTURE_MODAL_MOVE). Returns true when the event
 * was consumed.
 */
static bool gesture_move_handle(bContext *C, SelectGesture3DData &data, const wmEvent *event)
{
  const int2 mval(event->mval[0], event->mval[1]);
  if (event->type == EVT_SPACEKEY && ELEM(event->val, KM_PRESS, KM_RELEASE)) {
    data.move = (event->val == KM_PRESS);
    data.last_xy = mval;
    return true;
  }
  if (event->type == MOUSEMOVE && data.move) {
    const int2 delta_px = mval - data.last_xy;
    const float2 delta = float2(delta_px);
    data.last_xy = mval;
    data.press_xy += delta_px;
    data.box_region.xmin += delta.x;
    data.box_region.xmax += delta.x;
    data.box_region.ymin += delta.y;
    data.box_region.ymax += delta.y;
    data.circle_center += delta;
    for (float2 &p : data.path_region) {
      p += delta;
    }
    ED_region_tag_redraw(CTX_wm_region(C));
    return true;
  }
  return false;
}

static void gesture_data_free(SelectGesture3DData *data)
{
  if (data != nullptr) {
    MEM_delete(data);
  }
}

/** True for the view-navigation input a gesture must pass through untouched. */
static bool gesture_is_navigation(const wmEvent *event)
{
  if (ELEM(event->type, MIDDLEMOUSE, WHEELUPMOUSE, WHEELDOWNMOUSE, WHEELINMOUSE, WHEELOUTMOUSE)) {
    return true;
  }
  return event->type == MOUSEPAN || event->type == MOUSEZOOM || event->type == MOUSEROTATE ||
         event->type == NDOF_MOTION;
}

static float2 gesture_mval_f(const wmEvent *event)
{
  return float2(float(event->mval[0]), float(event->mval[1]));
}

static void gesture_status_box(bContext *C, const SelectGesture3DData &data)
{
  const float width = data.box_region.xmax - data.box_region.xmin;
  const float height = data.box_region.ymax - data.box_region.ymin;
  char text[128];
  SNPRINTF(text, "Size: %.0f × %.0f px", width, height);
  ED_workspace_status_text(C, IFACE_(text));
}

static void gesture_status_circle(bContext *C, const SelectGesture3DData &data)
{
  char text[128];
  SNPRINTF(text, "Radius: %.0f px", double(data.circle_radius));
  ED_workspace_status_text(C, IFACE_(text));
}

/** Map a region-space rectangle to a plane-px quad (4 corners through the anchor). */
static bool gesture_box_to_plane_quad(const SelectGesture3DData &data,
                                      const ARegion &region,
                                      float2 r_quad[4])
{
  const int2 corners[4] = {int2(int(data.box_region.xmin), int(data.box_region.ymin)),
                           int2(int(data.box_region.xmax), int(data.box_region.ymin)),
                           int2(int(data.box_region.xmax), int(data.box_region.ymax)),
                           int2(int(data.box_region.xmin), int(data.box_region.ymax))};
  for (const int i : IndexRange(4)) {
    if (!image_select_view3d_mval_to_plane_px(data.anchor, region, corners[i], r_quad[i])) {
      return false;
    }
  }
  return true;
}

/** Rasterize the quad / polygon / circle the gesture described. */
static bool gesture_apply(bContext *C, wmOperator *op, SelectGesture3DData &data)
{
  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  if (region == nullptr || ob == nullptr) {
    return false;
  }
  eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  /* The keymap only maps Shift / Ctrl held at the press; holding them later (or while moving the
   * shape with Space) must still extend / subtract. An explicit mode of the tool is kept. */
  if (sel_op == SEL_OP_SET) {
    if (data.modifier_seen & KM_SHIFT) {
      sel_op = SEL_OP_ADD;
    }
    else if (data.modifier_seen & KM_CTRL) {
      sel_op = SEL_OP_SUB;
    }
  }

  /* The gesture geometry is mapped onto the anchor plane once: the inside test below runs per
   * projected texel and must not redo the screen->plane mapping per pixel. */
  Vector<float2> polygon;
  float2 circle_center = float2(0.0f);
  float circle_radius = 0.0f;
  const char *undo_name = "Select";
  PaintSelectionEdgePolicy edge_policy = BKE_image_paint_selection_edge_policy_hard();

  float2 scene_a, scene_b;
  if (gesture_is_scene_shape(data) && !gesture_scene_plane_points(data, *region, scene_a, scene_b))
  {
    return false;
  }
  if (data.kind == GestureKind::Box) {
    if (gesture_is_scene_shape(data)) {
      Vector<float2> outline;
      gesture_scene_outline(data, scene_a, scene_b, outline);
      polygon.extend(outline.as_span());
    }
    else {
      float2 quad[4];
      if (!gesture_box_to_plane_quad(data, *region, quad)) {
        return false;
      }
      polygon.extend(quad, 4);
    }
    undo_name = "Box Select";
  }
  else if (data.kind == GestureKind::Lasso || data.kind == GestureKind::Polyline) {
    if (data.path_region.size() < 3) {
      return false;
    }
    polygon.resize(data.path_region.size());
    for (const int i : data.path_region.index_range()) {
      float2 p;
      if (!image_select_view3d_mval_to_plane_px(
              data.anchor, *region, int2(int(data.path_region[i].x), int(data.path_region[i].y)), p))
      {
        return false;
      }
      polygon[i] = p;
    }
    undo_name = "Lasso Select";
    edge_policy = BKE_image_paint_selection_edge_policy_feathered();
  }
  else { /* Circle. */
    if (gesture_is_scene_shape(data)) {
      /* The radius is measured on the plane, so the circle stays a circle of the surface. */
      circle_center = scene_a;
      circle_radius = std::max(math::distance(scene_a, scene_b), 1.0f);
    }
    else if (!image_select_view3d_mval_to_plane_px(data.anchor,
                                                   *region,
                                                   int2(int(data.circle_center.x),
                                                        int(data.circle_center.y)),
                                                   circle_center))
    {
      return false;
    }
    else {
      circle_radius = std::max(data.circle_radius, 1.0f);
    }
    undo_name = "Circle Select";
    edge_policy = BKE_image_paint_selection_edge_policy_feathered();
  }

  rctf domain;
  BLI_rctf_init(&domain, FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX);
  if (!polygon.is_empty()) {
    for (const float2 &p : polygon) {
      domain.xmin = std::min(domain.xmin, p.x);
      domain.xmax = std::max(domain.xmax, p.x);
      domain.ymin = std::min(domain.ymin, p.y);
      domain.ymax = std::max(domain.ymax, p.y);
    }
  }
  else {
    domain.xmin = circle_center.x - circle_radius;
    domain.xmax = circle_center.x + circle_radius;
    domain.ymin = circle_center.y - circle_radius;
    domain.ymax = circle_center.y + circle_radius;
  }

  return image_select_view3d_gesture_apply(
      C,
      *ob,
      data.anchor,
      domain,
      [&](const float2 &plane_px) {
        if (!polygon.is_empty()) {
          const float pt[2] = {plane_px.x, plane_px.y};
          return isect_point_poly_v2(pt,
                                     reinterpret_cast<const float (*)[2]>(polygon.data()),
                                     uint(polygon.size()));
        }
        return math::distance(plane_px, circle_center) <= circle_radius;
      },
      edge_policy,
      undo_name,
      sel_op);
}

static void gesture_finish_common(bContext *C, wmOperator *op)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data != nullptr && data->draw_handle != nullptr && data->owner_region_type != nullptr) {
    ED_region_draw_cb_exit(data->owner_region_type, data->draw_handle);
  }
  gesture_data_free(data);
  op->customdata = nullptr;
  ED_workspace_status_text(C, nullptr);
  WM_cursor_modal_restore(CTX_wm_window(C));
}

static wmOperatorStatus gesture_invoke_common(bContext *C,
                                              wmOperator *op,
                                              const wmEvent *event,
                                              const GestureKind kind)
{
  /* A floating fragment under the cursor is moved, not re-selected (2D parity). Checked BEFORE
   * the general poll, which refuses while a session is live. */
  if (PaintSelectView3DFloatingSession *floating = image_select_view3d_session_active()) {
    if (floating->owner_v3d == CTX_wm_view3d(C) &&
        floating->cursor_over_fragment(C, int2(event->mval[0], event->mval[1])))
    {
      WM_operator_name_call(
          C, "PAINT_OT_image_select_view3d_move", wm::OpCallContext::InvokeDefault, nullptr, event);
      return OPERATOR_FINISHED;
    }
    /* A live session of another tool is committed first, like the 2D gestures do. */
    image_select_view3d_session_end(C, true);
  }
  if (!image_paint_selection_view3d_poll(C)) {
    return OPERATOR_PASS_THROUGH;
  }

  auto *data = MEM_new<SelectGesture3DData>(__func__);
  data->kind = kind;
  /* The shared tool setting picks View (screen-space shape) or Scene (shape laid on the surface)
   * for the Box and Circle gestures; Lasso and Polyline always lay their path on the surface. */
  const Scene *scene = CTX_data_scene(C);
  const bool use_screen_space = ELEM(kind, GestureKind::Box, GestureKind::Circle) &&
                                scene != nullptr &&
                                scene->toolsettings->imapaint.selection_space ==
                                    IMAGE_PAINT_SELECT_SPACE_VIEW;
  if (!image_select_view3d_anchor_from_mval(
          C, int2(event->mval[0], event->mval[1]), data->anchor, use_screen_space))
  {
    gesture_data_free(data);
    BKE_report(op->reports, RPT_WARNING, "Selection: no surface under the cursor");
    return OPERATOR_CANCELLED;
  }
  data->press_xy = int2(event->mval[0], event->mval[1]);
  data->last_xy = data->press_xy;
  data->modifier_seen = event->modifier;
  if (kind == GestureKind::Circle) {
    data->circle_center = gesture_mval_f(event);
  }
  if (kind == GestureKind::Polyline) {
    data->path_region.append(gesture_mval_f(event));
  }
  op->customdata = data;
  if (ARegion *region = CTX_wm_region(C)) {
    data->owner_region_type = region->runtime->type;
    data->draw_handle = ED_region_draw_cb_activate(
        data->owner_region_type, gesture_draw_preview, data, REGION_DRAW_POST_PIXEL);
  }
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  ED_workspace_status_text(C, nullptr);
  return OPERATOR_RUNNING_MODAL;
}

/** Shared modal tail: confirm on release / cancel, navigation pass-through. */
static wmOperatorStatus gesture_modal_common(bContext * /*C*/,
                                             wmOperator * /*op*/,
                                             const wmEvent *event,
                                             SelectGesture3DData &data)
{
  /* Shift / Ctrl held at the start or at any later event (e.g. while moving the shape with Space,
   * which the keymap cannot know) extend / subtract, see #gesture_apply. */
  data.modifier_seen |= event->modifier;
  if (gesture_is_navigation(event)) {
    return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus view3d_select_box_invoke(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Box);
}

static wmOperatorStatus view3d_select_box_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      const float2 mval = gesture_mval_f(event);
      data->last_xy = int2(event->mval[0], event->mval[1]);
      if (math::distance(mval, float2(data->press_xy)) >
          float(SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX))
      {
        data->is_simple_click = false;
        data->box_valid = true;
      }
      data->box_region.xmin = std::min(float(data->press_xy.x), mval.x);
      data->box_region.xmax = std::max(float(data->press_xy.x), mval.x);
      data->box_region.ymin = std::min(float(data->press_xy.y), mval.y);
      data->box_region.ymax = std::max(float(data->press_xy.y), mval.y);
      gesture_status_box(C, *data);
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        Object *ob = CTX_data_active_object(C);
        const bool applied = data->box_valid && ob != nullptr &&
                             gesture_apply(C, op, *data);
        const bool simple = data->is_simple_click;
        gesture_finish_common(C, op);
        if (simple && ob != nullptr) {
          view3d_apply_deselect(C, *ob);
        }
        return applied || simple ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_box(wmOperatorType *ot)
{
  ot->name = "Box Select";
  ot->idname = "PAINT_OT_image_select_view3d_box";
  ot->description = "Select a box region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_box_invoke;
  ot->modal = view3d_select_box_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

static wmOperatorStatus view3d_select_lasso_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Lasso);
}

static wmOperatorStatus view3d_select_lasso_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      const float2 mval = gesture_mval_f(event);
      if (math::distance(mval, float2(data->press_xy)) >
          float(SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX))
      {
        data->is_simple_click = false;
      }
      if (data->path_region.is_empty() ||
          math::distance(mval, data->path_region.last()) >= SELECT_VIEW3D_PATH_MIN_STEP_PX)
      {
        data->path_region.append(mval);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        Object *ob = CTX_data_active_object(C);
        const bool simple = data->is_simple_click;
        const bool applied = !simple && ob != nullptr && gesture_apply(C, op, *data);
        gesture_finish_common(C, op);
        if (simple && ob != nullptr) {
          view3d_apply_deselect(C, *ob);
        }
        return applied || simple ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_lasso(wmOperatorType *ot)
{
  ot->name = "Lasso Select";
  ot->idname = "PAINT_OT_image_select_view3d_lasso";
  ot->description = "Select a lasso region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_lasso_invoke;
  ot->modal = view3d_select_lasso_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

static wmOperatorStatus view3d_select_polyline_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Polyline);
}

static wmOperatorStatus view3d_select_polyline_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      /* Refreshed for the preview draw: the segment from the last committed point to the live
       * cursor is what the user aims with between clicks. */
      data->last_xy = int2(int(event->mval[0]), int(event->mval[1]));
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        const float2 mval = gesture_mval_f(event);
        if (!data->path_region.is_empty() &&
            math::distance(mval, data->path_region.first()) <
                float(SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX))
        {
          /* Clicking the first point closes the polygon. */
          if (data->path_region.size() >= 3 && gesture_apply(C, op, *data)) {
            gesture_finish_common(C, op);
            return OPERATOR_FINISHED;
          }
          gesture_finish_common(C, op);
          return OPERATOR_CANCELLED;
        }
        data->path_region.append(mval);
        ED_region_tag_redraw(CTX_wm_region(C));
      }
      break;
    case EVT_RETKEY:
    case EVT_PADENTER:
      if (event->val == KM_PRESS) {
        if (data->path_region.size() >= 3 && gesture_apply(C, op, *data)) {
          gesture_finish_common(C, op);
          return OPERATOR_FINISHED;
        }
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_polyline(wmOperatorType *ot)
{
  ot->name = "Polyline Select";
  ot->idname = "PAINT_OT_image_select_view3d_polyline";
  ot->description = "Select a polyline region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_polyline_invoke;
  ot->modal = view3d_select_polyline_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

static wmOperatorStatus view3d_select_circle_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  return gesture_invoke_common(C, op, event, GestureKind::Circle);
}

static wmOperatorStatus view3d_select_circle_modal(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  auto *data = static_cast<SelectGesture3DData *>(op->customdata);
  if (data == nullptr) {
    return OPERATOR_CANCELLED;
  }
  wmOperatorStatus base = gesture_modal_common(C, op, event, *data);
  if (base != OPERATOR_RUNNING_MODAL) {
    return base;
  }
  if (gesture_move_handle(C, *data, event)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (event->type) {
    case MOUSEMOVE: {
      data->last_xy = int2(event->mval[0], event->mval[1]);
      data->circle_radius = math::distance(gesture_mval_f(event), data->circle_center);
      data->is_simple_click = data->circle_radius <=
                              float(SELECT_VIEW3D_CLICK_DRAG_THRESHOLD_PX);
      gesture_status_circle(C, *data);
      ED_region_tag_redraw(CTX_wm_region(C));
      break;
    }
    case LEFTMOUSE:
      if (event->val == KM_RELEASE) {
        Object *ob = CTX_data_active_object(C);
        const bool simple = data->is_simple_click;
        const bool applied = !simple && ob != nullptr && gesture_apply(C, op, *data);
        gesture_finish_common(C, op);
        if (simple && ob != nullptr) {
          view3d_apply_deselect(C, *ob);
        }
        return applied || simple ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      break;
    case EVT_ESCKEY:
      if (event->val == KM_PRESS) {
        gesture_finish_common(C, op);
        return OPERATOR_CANCELLED;
      }
      break;
    default:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void PAINT_OT_image_select_view3d_circle(wmOperatorType *ot)
{
  ot->name = "Circle Select";
  ot->idname = "PAINT_OT_image_select_view3d_circle";
  ot->description = "Select a circular region of the paint canvas drawn on the surface";
  ot->invoke = view3d_select_circle_invoke;
  ot->modal = view3d_select_circle_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
  WM_operator_properties_select_operation_simple(ot);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Floating session slot and canvas borrow
 * \{ */

namespace image_select_v3d {

/** One floating selection tool at a time, across all Viewports (the borrow guard makes a second
 * session on the same canvas impossible anyway). */
PaintSelectView3DFloatingSession *g_view3d_session = nullptr;

void borrow_images_set(const PaintSelectView3DFloatingSession &session,
                       const Span<Image *> images,
                       const void *borrower)
{
  for (Image *image : images) {
    if (image == nullptr || image->runtime == nullptr) {
      continue;
    }
    /* A file load / memfile undo may have freed and re-created this image while the session
     * floated: touching its runtime then would be a dangling write (and the re-created image's
     * fresh runtime carries no borrow anyway, so skipping is exactly right). */
    if (!image_select_view3d_image_is_alive(session, image)) {
      continue;
    }
    image->runtime->paint_selection_borrowed_by = borrower;
  }
}

}  // namespace image_select_v3d

PaintSelectView3DFloatingSession *image_select_view3d_session_active()
{
  return image_select_v3d::g_view3d_session;
}

bool image_select_view3d_image_is_alive(const PaintSelectView3DFloatingSession &session,
                                        const Image *image)
{
  if (image == nullptr || session.bmain == nullptr) {
    return false;
  }
  /* Compare pointers only: a freed image must not be dereferenced to read its session UID. */
  for (const Image &candidate : session.bmain->images) {
    if (&candidate == image) {
      return true;
    }
  }
  return false;
}

void image_select_view3d_session_set(bContext *C,
                                     View3D *owner_v3d,
                                     PaintSelectView3DFloatingSession *session)
{
  BLI_assert(image_select_v3d::g_view3d_session == nullptr);
  BLI_assert(session != nullptr && owner_v3d != nullptr);
  session->owner_v3d = owner_v3d;
  session->owner_region = CTX_wm_region(C);
  session->owner_region_type =
      CTX_wm_region(C) != nullptr ? CTX_wm_region(C)->runtime->type : nullptr;
  session->bmain = CTX_data_main(C);
  image_select_v3d::g_view3d_session = session;
  image_select_v3d::borrow_images_set(*session, session->borrowed_images, owner_v3d);
}

void image_select_view3d_floating_draw_handle_clear(PaintSelectView3DFloatingSession &session)
{
  if (session.draw_handle != nullptr && session.owner_region_type != nullptr) {
    ED_region_draw_cb_exit(session.owner_region_type, session.draw_handle);
    session.draw_handle = nullptr;
  }
}

bool image_select_view3d_session_end(bContext *C, const bool commit)
{
  PaintSelectView3DFloatingSession *session = image_select_v3d::g_view3d_session;
  if (session == nullptr) {
    return false;
  }
  image_select_v3d::g_view3d_session = nullptr;
  /* Un-borrow before the tool writes its final result, so the commit is an ordinary selection
   * edit again (the borrow only blocks foreign edits, not the owner's own commit). */
  image_select_v3d::borrow_images_set(*session, session->borrowed_images, nullptr);
  if (commit) {
    session->commit(C);
  }
  else {
    session->cancel(C);
  }
  MEM_delete(session);
  if (C != nullptr) {
    WM_event_add_notifier(C, NC_WINDOW, nullptr);
  }
  return true;
}

void image_select_view3d_space_free(View3D *v3d)
{
  PaintSelectView3DFloatingSession *session = image_select_v3d::g_view3d_session;
  if (session == nullptr || session->owner_v3d != v3d) {
    return;
  }
  image_select_v3d::g_view3d_session = nullptr;
  image_select_v3d::borrow_images_set(*session, session->borrowed_images, nullptr);
  session->cancel(nullptr);
  MEM_delete(session);
}

bool image_select_view3d_canvas_borrowed_elsewhere(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  return image_select_view3d_canvas_borrowed_elsewhere(
      C, image_paint_selection_view3d_targets_get(C, *ob));
}

bool image_select_view3d_canvas_borrowed_elsewhere(bContext *C,
                                                   const Span<ImagePaintSelectionTarget> targets)
{
  const void *borrower = CTX_wm_view3d(C);
  for (const ImagePaintSelectionTarget &target : targets) {
    if (target.image == nullptr || target.image->runtime == nullptr) {
      continue;
    }
    const void *borrowed_by = target.image->runtime->paint_selection_borrowed_by;
    if (borrowed_by != nullptr && borrowed_by != borrower) {
      return true;
    }
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Shared floating-session operators
 *
 * One confirm / cancel / undo-step operator per session instead of per tool: they dispatch
 * through the live session's virtual #PaintSelectView3DFloatingSession::commit / #cancel /
 * #undo_step, whichever tool is running.
 * \{ */

static bool image_select_view3d_floating_session_poll(bContext *C)
{
  const PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  return session != nullptr && session->owner_v3d == CTX_wm_view3d(C);
}

static wmOperatorStatus image_select_view3d_floating_confirm_exec(bContext *C,
                                                                  wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_select_view3d_session_end(C, true);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_view3d_floating_cancel_exec(bContext *C,
                                                                 wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_select_view3d_session_end(C, false);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_view3d_floating_undo_step_exec(bContext *C,
                                                                    wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  if (!session->undo_step()) {
    /* Nothing left to undo inside the session: Ctrl+Z ends it with a cancel. */
    image_select_view3d_session_end(C, false);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_view3d_floating_confirm(wmOperatorType *ot)
{
  ot->name = "Confirm Floating Selection";
  ot->idname = "PAINT_OT_image_select_view3d_floating_confirm";
  ot->description = "Apply the floating selection edit and end the session";

  ot->exec = image_select_view3d_floating_confirm_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

void PAINT_OT_image_select_view3d_floating_cancel(wmOperatorType *ot)
{
  ot->name = "Cancel Floating Selection";
  ot->idname = "PAINT_OT_image_select_view3d_floating_cancel";
  ot->description = "Restore the pre-session state and end the floating selection session";

  ot->exec = image_select_view3d_floating_cancel_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

void PAINT_OT_image_select_view3d_floating_undo_step(wmOperatorType *ot)
{
  ot->name = "Undo Floating Selection Step";
  ot->idname = "PAINT_OT_image_select_view3d_floating_undo_step";
  ot->description =
      "Revert the last drag of the floating selection; with nothing left, end the session";

  ot->exec = image_select_view3d_floating_undo_step_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

/** Cycle the on-surface preview between the canvas images of the live session. */
static wmOperatorStatus image_select_view3d_preview_channel_exec(bContext *C,
                                                                 wmOperator * /*op*/)
{
  if (!image_select_view3d_floating_session_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  session->preview_channel_cycle();
  if (session->owner_region != nullptr) {
    ED_region_tag_redraw(session->owner_region);
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_view3d_preview_channel(wmOperatorType *ot)
{
  ot->name = "Cycle Selection Preview Channel";
  ot->idname = "PAINT_OT_image_select_view3d_preview_channel";
  ot->description =
      "Cycle which of the material's paint canvases (base color, roughness, ...) the floating "
      "fragment preview shows on the surface";

  ot->exec = image_select_view3d_preview_channel_exec;
  ot->poll = image_select_view3d_floating_session_poll;
  ot->flag = 0;
}

/** \} */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface map (UV -> surface sampling)
 * \{ */

bool ImageSelectView3DSurfaceMap::ensure(const Object &ob, const StringRef uv_name)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr) {
    return false;
  }
  /* Identity guards: rebuild when the object, the UV map or the topology changed. */
  const uint32_t ob_uid = uint32_t(ob.id.session_uid);
  const int64_t corner_tri_num = int64_t(mesh->corner_tris().size());
  if (this->sampler != nullptr && this->object_session_uid == ob_uid &&
      this->corner_tri_num == corner_tri_num && uv_name == this->uv_map_name)
  {
    return true;
  }
  const std::string name = uv_name.is_empty() ? std::string(mesh->active_or_default_uv_map_name()) :
                                                std::string(uv_name);
  if (name.empty()) {
    return false;
  }
  const bke::AttributeReader<float2> uv_attribute = mesh->attributes().lookup<float2>(
      name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return false;
  }
  const VArraySpan<float2> uv_span(*uv_attribute);
  const Span<int3> corner_tris = mesh->corner_tris();

  this->uv_map = Array<float2>(uv_span);
  this->corner_tris = Array<int3>(corner_tris);
  this->sampler = std::make_unique<geometry::ReverseUVSampler>(this->uv_map.as_span(),
                                                               this->corner_tris.as_span());
  this->object_session_uid = ob_uid;
  this->corner_tri_num = corner_tri_num;
  this->uv_map_name = name;
  return true;
}

bool ImageSelectView3DSurfaceMap::sample(const Span<int> corner_verts,
                                         const Span<float3> vert_positions,
                                         const float2 &uv,
                                         float3 &r_co,
                                         float3 &r_no) const
{
  if (this->sampler == nullptr) {
    return false;
  }
  const geometry::ReverseUVSampler::Result result = this->sampler->sample(uv);
  if (result.type == geometry::ReverseUVSampler::ResultType::None ||
      result.tri_index < 0 || result.tri_index >= this->corner_tris.size())
  {
    return false;
  }
  const int3 tri = this->corner_tris[result.tri_index];
  if (tri.x >= corner_verts.size() || tri.y >= corner_verts.size() ||
      tri.z >= corner_verts.size())
  {
    return false;
  }
  const int3 verts = int3(corner_verts[tri.x], corner_verts[tri.y], corner_verts[tri.z]);
  if (verts.x >= vert_positions.size() || verts.y >= vert_positions.size() ||
      verts.z >= vert_positions.size())
  {
    return false;
  }
  const float3 bary = result.bary_weights;
  r_co = vert_positions[verts.x] * bary.x + vert_positions[verts.y] * bary.y +
         vert_positions[verts.z] * bary.z;
  const float3 e1 = vert_positions[verts.y] - vert_positions[verts.x];
  const float3 e2 = vert_positions[verts.z] - vert_positions[verts.x];
  r_no = math::normalize(math::cross(e1, e2));
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask boundary in UV space
 * \{ */

void image_select_mask_boundary_segments(const float *data,
                                         const int2 size,
                                         const int2 scan_min,
                                         const int2 scan_max,
                                         const FunctionRef<float2(const int2 &)> uv_cell,
                                         const int stride,
                                         Vector<float2> &r_segments)
{
  const int stride_clamped = std::max(stride, 1);
  const int x0 = std::clamp(scan_min.x, 0, size.x - 1);
  const int x1 = std::clamp(scan_max.x, 0, size.x);
  const int y0 = std::clamp(scan_min.y, 0, size.y - 1);
  const int y1 = std::clamp(scan_max.y, 0, size.y);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  const auto selected = [&](const int x, const int y) {
    return x >= 0 && y >= 0 && x < size.x && y < size.y &&
           data[int64_t(y) * size.x + x] > SELECTION_MASK_THRESHOLD;
  };

  /* Rows are independent: each worker accumulates its own segments, concatenated afterwards
   * (segment order does not matter, every line is surface-mapped independently). */
  const int row_count = ((y1 - y0 - 1) / stride_clamped) + 1;
  threading::EnumerableThreadSpecific<Vector<float2>> all_tls;
  threading::parallel_for(IndexRange(row_count), 64, [&](const IndexRange rows) {
    Vector<float2> &local = all_tls.local();
    for (const int r : rows) {
      const int y = y0 + r * stride_clamped;
      for (int x = x0; x < x1; x += stride_clamped) {
        if (!selected(x, y)) {
          continue;
        }
        if (!selected(x - 1, y)) {
          local.append(uv_cell(int2(x, y)));
          local.append(uv_cell(int2(x, y + 1)));
        }
        if (!selected(x + 1, y)) {
          local.append(uv_cell(int2(x + 1, y)));
          local.append(uv_cell(int2(x + 1, y + 1)));
        }
        if (!selected(x, y - 1)) {
          local.append(uv_cell(int2(x, y)));
          local.append(uv_cell(int2(x + 1, y)));
        }
        if (!selected(x, y + 1)) {
          local.append(uv_cell(int2(x, y + 1)));
          local.append(uv_cell(int2(x + 1, y + 1)));
        }
      }
    }
  });
  for (const Vector<float2> &local : all_tls) {
    r_segments.extend(local);
  }
}

Vector<float2> image_select_view3d_mask_boundary_uv(const Image &image,
                                                    const int tile_number,
                                                    const ImageUser & /*iuser*/,
                                                    const int step)
{
  Vector<float2> segments;
  const ImBuf *mask = BKE_image_paint_selection_mask_lookup(&image, tile_number);
  if (mask == nullptr || mask->float_data() == nullptr) {
    return segments;
  }
  const float *data = mask->float_data();
  const int w = mask->x;
  const int h = mask->y;
  const float2 uv_origin = BKE_image_get_tile_uv_origin(tile_number);
  const auto uv_cell = [&](const int2 &px) {
    return uv_origin + float2(float(px.x) / float(w), float(px.y) / float(h));
  };

  /* Scan only the selection's bounding box (memoized in BKE against the mask revision): the
   * boundary cannot lie outside it, and an 8K tile is mostly empty around a small selection. */
  int2 scan_min(0, 0);
  int2 scan_max(w, h);
  int bounds_min[2];
  int bounds_max[2];
  if (BKE_image_paint_selection_mask_bounds(&image, tile_number, bounds_min, bounds_max)) {
    scan_min = int2(bounds_min[0], bounds_min[1]);
    scan_max = int2(bounds_max[0], bounds_max[1]);
  }
  image_select_mask_boundary_segments(data, int2(w, h), scan_min, scan_max, uv_cell, step,
                                      segments);
  return segments;
}

void image_select_view3d_draw_uv_outlines(const Object &ob,
                                          const ImageSelectView3DSurfaceMap &surface_map,
                                          const Span<Vector<float2>> uv_polylines,
                                          const float color[4],
                                          const float dash_width)
{
  if (uv_polylines.is_empty()) {
    return;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr) {
    return;
  }
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = mesh->vert_positions();

  float4x4 object_to_world(ob.object_to_world());

  int64_t total_points = 0;
  for (const Vector<float2> &poly : uv_polylines) {
    total_points += int64_t(poly.size());
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_width(1.0f);
  const uint pos_3d = GPU_vertformat_attr_add(immVertexFormat(), "pos",
                                              gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_LINE_DASHED_UNIFORM_COLOR);
  float viewport_size[4];
  GPU_viewport_size_get_f(viewport_size);
  immUniform2f("viewport_size", viewport_size[2], viewport_size[3]);
  immUniform1i("colors_len", 2); /* Two-tone dash. */
  immUniform4f("color", color[0], color[1], color[2], color[3]);
  immUniform4f("color2", 1.0f, 1.0f, 1.0f, 1.0f);
  immUniform1f("dash_width", dash_width);
  immUniform1f("udash_factor", 0.5f);

  /* The inputs are segment pairs (#image_select_mask_boundary_segments), not connected
   * polylines: chaining consecutive points would join unrelated boundary segments with long
   * lines. Segments whose ends miss the surface are dropped, so the vertex count is only known
   * after sampling and #immBegin must receive exactly that count. */
  Vector<float3> line_verts;
  line_verts.reserve(total_points);
  for (const Vector<float2> &poly : uv_polylines) {
    for (int64_t i = 0; i + 1 < poly.size(); i += 2) {
      float3 co_a, no_a, co_b, no_b;
      if (!surface_map.sample(corner_verts, vert_positions, poly[i], co_a, no_a)) {
        continue;
      }
      if (!surface_map.sample(corner_verts, vert_positions, poly[i + 1], co_b, no_b)) {
        continue;
      }
      line_verts.append(math::transform_point(object_to_world, co_a));
      line_verts.append(math::transform_point(object_to_world, co_b));
    }
  }
  if (!line_verts.is_empty()) {
    immBegin(GPU_PRIM_LINES, uint(line_verts.size()));
    for (const float3 &v : line_verts) {
      immVertex3f(pos_3d, v.x, v.y, v.z);
    }
    immEnd();
  }
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

void image_select_view3d_draw_uv_texture_ext(
    const Object &ob,
    const ImageSelectView3DSurfaceMap &surface_map,
    gpu::Texture *texture,
    const float2 &dest_rect_min,
    const float2 &dest_rect_max,
    const FunctionRef<float2(const float2 &dest_uv)> &dest_uv_to_texco)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr || texture == nullptr || surface_map.uv_map.is_empty() ||
      dest_rect_min.x >= dest_rect_max.x || dest_rect_min.y >= dest_rect_max.y)
  {
    return;
  }
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = mesh->vert_positions();
  const float4x4 object_to_world(ob.object_to_world());

  /* Every mesh triangle whose UV bounds touch the rect, with the texture coordinate derived from
   * its own UVs through \a dest_uv_to_texco: the fragment is drawn on the surface exactly where
   * the commit will write it. */
  Vector<float3> positions;
  Vector<float2> tex_coords;
  for (const int3 &tri : surface_map.corner_tris) {
    const float2 uv0 = surface_map.uv_map[tri.x];
    const float2 uv1 = surface_map.uv_map[tri.y];
    const float2 uv2 = surface_map.uv_map[tri.z];
    const float2 tri_min = math::min(uv0, math::min(uv1, uv2));
    const float2 tri_max = math::max(uv0, math::max(uv1, uv2));
    if (tri_max.x < dest_rect_min.x || tri_min.x > dest_rect_max.x ||
        tri_max.y < dest_rect_min.y || tri_min.y > dest_rect_max.y)
    {
      continue;
    }
    for (const int c : IndexRange(3)) {
      positions.append(math::transform_point(object_to_world,
                                             vert_positions[corner_verts[tri[c]]]));
      tex_coords.append(dest_uv_to_texco(surface_map.uv_map[tri[c]]));
    }
  }
  if (positions.is_empty()) {
    return;
  }

  GPU_blend(GPU_BLEND_ALPHA);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint texco = GPU_vertformat_attr_add(format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  /* Clamp to the (transparent) border: triangles that only partly overlap the rect sample outside
   * [0, 1] and must stay invisible there. */
  immBindTextureSampler("image",
                        texture,
                        {GPU_SAMPLER_FILTERING_LINEAR,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER,
                         GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER});
  immBegin(GPU_PRIM_TRIS, uint(positions.size()));
  for (const int64_t i : positions.index_range()) {
    immAttr2f(texco, tex_coords[i].x, tex_coords[i].y);
    immVertex3f(pos, positions[i].x, positions[i].y, positions[i].z);
  }
  immEnd();
  immUnbindProgram();
  GPU_blend(GPU_BLEND_NONE);
}

void image_select_view3d_draw_uv_texture(const Object &ob,
                                         const ImageSelectView3DSurfaceMap &surface_map,
                                         gpu::Texture *texture,
                                         const float2 &uv_min,
                                         const float2 &uv_size)
{
  if (uv_size.x <= 0.0f || uv_size.y <= 0.0f) {
    return;
  }
  image_select_view3d_draw_uv_texture_ext(
      ob, surface_map, texture, uv_min, uv_min + uv_size, [&](const float2 &dest_uv) {
        return (dest_uv - uv_min) / uv_size;
      });
}

void image_select_view3d_preview_textures_free(Vector<Vector<gpu::Texture *>> &textures)
{
  bool any = false;
  for (const Vector<gpu::Texture *> &per_target : textures) {
    for (const gpu::Texture *tex : per_target) {
      any = any || tex != nullptr;
    }
  }
  if (!any) {
    textures.clear();
    return;
  }
  const bool context_enabled = DRW_gpu_context_is_enabled();
  if (!context_enabled) {
    DRW_gpu_context_enable();
  }
  for (Vector<gpu::Texture *> &per_target : textures) {
    for (gpu::Texture *tex : per_target) {
      if (tex != nullptr) {
        GPU_texture_free(tex);
      }
    }
  }
  if (!context_enabled) {
    /* Restores the window's drawable: this runs between redraws (session end after a commit),
     * and leaving the GPU context unset crashes the next #wm_draw_update. */
    DRW_gpu_context_disable();
  }
  textures.clear();
}

void image_select_view3d_preview_texture_free(gpu::Texture *&texture)
{
  if (texture == nullptr) {
    return;
  }
  const bool context_enabled = DRW_gpu_context_is_enabled();
  if (!context_enabled) {
    DRW_gpu_context_enable();
  }
  GPU_texture_free(texture);
  texture = nullptr;
  if (!context_enabled) {
    /* Restores the window's drawable: see #image_select_view3d_preview_textures_free. */
    DRW_gpu_context_disable();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 3D overlay (marching-ants outline projected onto the surface)
 * \{ */

namespace image_select_v3d {

/**
 * The mask as a GPU 2D-array texture (one layer per mask-bearing tile) plus the UV triangles that
 * cover the selection's neighborhoods. The boundary shader samples the mask through the mesh UVs
 * and draws the 0.5-crossing as a screen-space-dashed line: no CPU boundary extraction, the
 * outline follows the surface exactly, and depth testing works because the overlay is drawn on
 * the surface itself.
 */
struct OverlayCache {
  const Image *image = nullptr;
  uint64_t revision = 0;
  uint32_t ob_uid = 0;
  int64_t corner_tri_num = 0;
  std::string uv_map_name;
  /** Mask texture (SFLOAT_16, #texture_size per layer) and the tile number of each layer. */
  gpu::Texture *mask_texture = nullptr;
  Vector<int> layer_tiles;
  int2 texture_size = int2(0);
  /** Region uploaded into each texture layer last time (invalid when nothing was uploaded): the
   * next upload covers its union with the new bounds, which is where the mask may have changed
   * (a change either adds a selected pixel — inside the new bounds — or clears one — inside the
   * previous ones). */
  Vector<rcti> layer_uploaded;
  /** Triangle batch: interleaved world position, global UV and tile layer per corner. */
  gpu::VertBuf *tris_verts = nullptr;
  gpu::Batch *tris_batch = nullptr;
  /** Corner indices of every emitted triangle (3 each) for the position refreshes. */
  Vector<int3> tri_corners;
  /** CPU mirrors of the static attributes (re-filled on every position upload). */
  Vector<float2> tri_uvs;
  Vector<float> tri_layers;
  /** UV bounds of every corner triangle (min.xy, max.zw in global UV) and the mesh identity they
   * were computed from: the per-rebuild triangle filter is then a plain scan over them instead of
   * an attribute walk with per-triangle min/max. */
  Array<float4> tri_uv_bounds;
  uint32_t tri_bounds_ob_uid = 0;
  const Mesh *tri_bounds_mesh = nullptr;
  int64_t tri_bounds_corner_tri_num = 0;
  std::string tri_bounds_uv_map_name;
  /** PBVH positions-changed counter and object transform at the last position upload. */
  int64_t positions_changed_count = -1;
  float4x4 object_to_world = float4x4::identity();
};

OverlayCache &overlay_cache_get()
{
  static OverlayCache cache;
  return cache;
}

/** The boundary shader; registered as a builtin so its lifetime is managed by the GPU module
 * (created on first use here, freed with the backend on exit). */
gpu::Shader *overlay_shader_get()
{
  return GPU_shader_get_builtin_shader(GPU_SHADER_IMAGE_SELECT_MASK);
}

/** Free every GPU resource of the cache. Must run with a current GPU context. */
void overlay_gpu_free(OverlayCache &cache)
{
  GPU_BATCH_DISCARD_SAFE(cache.tris_batch);
  GPU_VERTBUF_DISCARD_SAFE(cache.tris_verts);
  GPU_TEXTURE_FREE_SAFE(cache.mask_texture);
  cache.layer_tiles.clear();
  cache.layer_uploaded.clear();
  cache.tri_corners.clear();
  cache.tri_uvs.clear();
  cache.tri_layers.clear();
  cache.texture_size = int2(0);
  cache.positions_changed_count = -1;
}

/**
 * Sub-update every layer with the region that may have changed since the last upload: the union
 * of the previously uploaded bounds and the current selection bounds. That union covers every
 * possible change between two mask revisions — a change either adds a selected pixel, which
 * lands inside the new bounds, or clears one, which lands inside the previous ones — and the
 * mask values re-loaded there are authoritative, so no whole-texture clear is needed (a full 8K
 * UDIM clear per edit would dominate the rebuild cost).
 */
static void overlay_mask_texture_update(OverlayCache &cache, const Image &image)
{
  BLI_assert(cache.layer_uploaded.size() == cache.layer_tiles.size());
  for (const int layer : cache.layer_tiles.index_range()) {
    const int tile_number = cache.layer_tiles[layer];
    const ImBuf *mask = BKE_image_paint_selection_mask_lookup(&image, tile_number);
    if (mask == nullptr || mask->float_data() == nullptr) {
      /* The tile's mask was freed: its layer entry is stale until the layout scan of the current
       * rebuild removes it. */
      continue;
    }
    rcti new_bounds;
    int bounds_min[2];
    int bounds_max[2];
    if (BKE_image_paint_selection_mask_bounds(&image, tile_number, bounds_min, bounds_max)) {
      BLI_rcti_init(&new_bounds,
                    std::clamp(bounds_min[0], 0, mask->x - 1),
                    std::clamp(bounds_max[0], 0, mask->x),
                    std::clamp(bounds_min[1], 0, mask->y - 1),
                    std::clamp(bounds_max[1], 0, mask->y));
    }
    else {
      /* The selection left this tile entirely. */
      BLI_rcti_init(&new_bounds, 1, 0, 1, 0);
    }
    /* Re-upload the union of the last uploaded region (where selected texels may have been
     * cleared) and the new bounds. */
    rcti upload = new_bounds;
    if (BLI_rcti_is_valid(&cache.layer_uploaded[layer])) {
      if (BLI_rcti_is_valid(&upload)) {
        BLI_rcti_union(&upload, &cache.layer_uploaded[layer]);
      }
      else {
        /* The union with the empty sentinel would stretch the region to the tile origin. */
        upload = cache.layer_uploaded[layer];
      }
    }
    if (BLI_rcti_is_valid(&upload) && BLI_rcti_size_x(&upload) > 0 &&
        BLI_rcti_size_y(&upload) > 0)
    {
      const float *region = mask->float_data() + int64_t(upload.ymin) * mask->x + upload.xmin;
      GPU_texture_update_sub(cache.mask_texture,
                             GPU_DATA_FLOAT,
                             region,
                             upload.xmin,
                             upload.ymin,
                             layer,
                             BLI_rcti_size_x(&upload),
                             BLI_rcti_size_y(&upload),
                             1,
                             uint(mask->x));
    }
    /* Only the new bounds are stored: the union above already refreshed the previous region. */
    cache.layer_uploaded[layer] = new_bounds;
  }
}

/** Collect the UV triangles covering the selection's neighborhoods and record their static
 * (uv / layer) attributes. The position attribute is filled by #overlay_tri_positions_update. */
static void overlay_tri_topology_update(const Object &ob,
                                        const Image &image,
                                        OverlayCache &cache)
{
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  if (mesh == nullptr || cache.mask_texture == nullptr) {
    return;
  }
  const bke::AttributeReader<float2> uv_attribute = mesh->attributes().lookup<float2>(
      cache.uv_map_name, bke::AttrDomain::Corner);
  if (!uv_attribute) {
    return;
  }
  const VArraySpan<float2> uv_map(*uv_attribute);
  const Span<int3> corner_tris = mesh->corner_tris();

  /* UV bounds of each texture layer's selection, dilated by a small margin so the boundary
   * shader always sees mask values on both sides of the edge. */
  struct TileBox {
    float2 min;
    float2 max;
    int layer;
  };
  Vector<TileBox> boxes;
  for (const int layer : cache.layer_tiles.index_range()) {
    const int tile_number = cache.layer_tiles[layer];
    int bounds_min[2];
    int bounds_max[2];
    if (!BKE_image_paint_selection_mask_bounds(&image, tile_number, bounds_min, bounds_max)) {
      continue;
    }
    const float2 uv_origin = BKE_image_get_tile_uv_origin(tile_number);
    const float2 size = float2(cache.texture_size);
    TileBox box;
    box.min = uv_origin + (float2(float(bounds_min[0]), float(bounds_min[1])) - 2.0f) / size;
    box.max = uv_origin + (float2(float(bounds_max[0]), float(bounds_max[1])) + 2.0f) / size;
    box.layer = layer;
    boxes.append(box);
  }

  /* The per-triangle UV bounds depend only on the mesh and its UV map, not on the selection:
   * rebuild them just when that identity changes, so the filter below stays a cheap scan across
   * rebuilds on heavy meshes. Limitation: an in-place UV edit that keeps the mesh pointer, the
   * triangle count and the UV map name would leave the cached bounds stale. The overlay only
   * draws in sculpt mode where UVs do not change, so the key above is sufficient there. */
  const uint32_t ob_uid = uint32_t(ob.id.session_uid);
  if (cache.tri_uv_bounds.size() != int64_t(corner_tris.size()) ||
      cache.tri_bounds_ob_uid != ob_uid || cache.tri_bounds_mesh != mesh ||
      cache.tri_bounds_corner_tri_num != corner_tris.size() ||
      cache.tri_bounds_uv_map_name != cache.uv_map_name)
  {
    cache.tri_uv_bounds.reinitialize(corner_tris.size());
    for (const int i : corner_tris.index_range()) {
      const float2 uv0 = uv_map[corner_tris[i].x];
      const float2 uv1 = uv_map[corner_tris[i].y];
      const float2 uv2 = uv_map[corner_tris[i].z];
      cache.tri_uv_bounds[i] = float4(math::min(uv0, math::min(uv1, uv2)),
                                      math::max(uv0, math::max(uv1, uv2)));
    }
    cache.tri_bounds_ob_uid = ob_uid;
    cache.tri_bounds_mesh = mesh;
    cache.tri_bounds_corner_tri_num = corner_tris.size();
    cache.tri_bounds_uv_map_name = cache.uv_map_name;
  }

  cache.tri_corners.clear();
  cache.tri_uvs.clear();
  cache.tri_layers.clear();
  for (const int i : corner_tris.index_range()) {
    if (cache.tri_corners.size() >= SELECT_VIEW3D_OUTLINE_MAX_TRIS) {
      break;
    }
    const int3 tri = corner_tris[i];
    const float4 &tri_bounds = cache.tri_uv_bounds[i];
    for (const TileBox &box : boxes) {
      if (tri_bounds.z < box.min.x || tri_bounds.x > box.max.x || tri_bounds.w < box.min.y ||
          tri_bounds.y > box.max.y)
      {
        continue;
      }
      /* Triangle UVs may straddle the tile border only for broken UVs; the shader clamps such
       * fragments to this triangle's own tile. */
      cache.tri_corners.append(tri);
      cache.tri_uvs.extend({uv_map[tri.x], uv_map[tri.y], uv_map[tri.z]});
      cache.tri_layers.extend({float(box.layer), float(box.layer), float(box.layer)});
      break;
    }
  }
}

/** (Re-)create the triangle batch and fill its static attributes. */
static void overlay_tri_batch_ensure(OverlayCache &cache)
{
  const int64_t vert_count = cache.tri_corners.size() * 3;
  if (cache.tris_verts != nullptr && int64_t(cache.tris_verts->vertex_len) == vert_count) {
    /* Same size: keep the batch and the vertex buffer, every attribute is re-filled by
     * #overlay_tri_positions_update. */
    return;
  }
  GPU_BATCH_DISCARD_SAFE(cache.tris_batch);
  GPU_VERTBUF_DISCARD_SAFE(cache.tris_verts);

  if (vert_count == 0) {
    return;
  }

  GPUVertFormat format;
  GPU_vertformat_clear(&format);
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  GPU_vertformat_attr_add(&format, "uv", gpu::VertAttrType::SFLOAT_32_32);
  GPU_vertformat_attr_add(&format, "layer_in", gpu::VertAttrType::SFLOAT_32);
  cache.tris_verts = GPU_vertbuf_create_with_format_ex(format, GPU_USAGE_DYNAMIC);
  GPU_vertbuf_data_alloc(*cache.tris_verts, uint(vert_count));
  cache.tris_batch = GPU_batch_calloc();
  GPU_batch_init(cache.tris_batch, GPU_PRIM_TRIS, cache.tris_verts, nullptr);
}

/** Refresh the world-space triangle positions from the (deforming) evaluated mesh. */
static void overlay_tri_positions_update(const bContext *C,
                                         const Object &ob,
                                         OverlayCache &cache)
{
  if (cache.tris_verts == nullptr) {
    return;
  }
  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  const Span<int> corner_verts = mesh->corner_verts();
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval(
      *CTX_data_depsgraph_pointer(C), const_cast<Object &>(ob));
  const float4x4 object_to_world(ob.object_to_world());

  struct TriVert {
    float3 pos;
    float2 uv;
    float layer;
  };
  MutableSpan<TriVert> verts = cache.tris_verts->data<TriVert>();
  int64_t v = 0;
  for (const int64_t tri_i : cache.tri_corners.index_range()) {
    const int3 tri = cache.tri_corners[tri_i];
    for (const int c : IndexRange(3)) {
      verts[v].pos = math::transform_point(object_to_world,
                                           vert_positions[corner_verts[tri[c]]]);
      verts[v].uv = cache.tri_uvs[v];
      verts[v].layer = cache.tri_layers[v];
      v++;
    }
  }
  GPU_vertbuf_tag_dirty(cache.tris_verts);

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  cache.positions_changed_count = pbvh != nullptr ? pbvh->positions_changed_count() : -1;
  cache.object_to_world = object_to_world;
}

void overlay_rebuild(const bContext *C, const Object &ob, OverlayCache &cache)
{
  Scene *scene = CTX_data_scene(C);
  ImagePaintSelectionTarget primary = {};
  const bool has_target = image_paint_selection_view3d_primary_target_get(
      const_cast<bContext *>(C), const_cast<Object &>(ob), primary);
  Image *image = has_target ? primary.image : nullptr;

  const Mesh *mesh = id_cast<const Mesh *>(ob.data);
  const StringRef uv_name = BKE_paint_canvas_uvmap_name_get(
                                &scene->toolsettings->paint_mode, const_cast<Object *>(&ob))
                                .value_or(mesh != nullptr ? mesh->active_or_default_uv_map_name() :
                                                            "");
  cache.image = image;
  cache.revision = BKE_image_paint_selection_mask_revision_get(image);
  cache.ob_uid = uint32_t(ob.id.session_uid);
  cache.corner_tri_num = mesh ? mesh->corner_tris().size() : 0;
  cache.uv_map_name = std::string(uv_name);

  /* Mask texture: one layer per tile that carries a mask; tiles of a different resolution than
   * the first layer are skipped (UDIM tiles of one image share their resolution in practice). */
  int2 texture_size(0);
  Vector<int> layer_tiles;
  if (image != nullptr && uv_name.size() != 0) {
    for (const ImageTile *tile : ConstListBaseWrapper<ImageTile>(image->tiles)) {
      const ImBuf *mask = BKE_image_paint_selection_mask_lookup(image, tile->tile_number);
      if (mask == nullptr || mask->float_data() == nullptr) {
        continue;
      }
      if (texture_size.x == 0) {
        texture_size = int2(mask->x, mask->y);
      }
      if (mask->x != texture_size.x || mask->y != texture_size.y) {
        continue;
      }
      layer_tiles.append(tile->tile_number);
    }
  }

  /* Keep the texture across rebuilds whenever the tile layout and resolution are unchanged: a
   * recreated 8K UDIM array would turn every rebuild into an allocation spike. */
  const bool texture_reusable = cache.mask_texture != nullptr &&
                                cache.texture_size == texture_size &&
                                cache.layer_tiles == layer_tiles;
  if (!texture_reusable) {
    overlay_gpu_free(cache);
    if (layer_tiles.is_empty()) {
      return;
    }
    cache.layer_tiles = layer_tiles;
    cache.texture_size = texture_size;
    cache.mask_texture = GPU_texture_create_2d_array("paint_selection_mask",
                                                     texture_size.x,
                                                     texture_size.y,
                                                     cache.layer_tiles.size(),
                                                     1,
                                                     gpu::TextureFormat::SFLOAT_16,
                                                     GPU_TEXTURE_USAGE_SHADER_READ,
                                                     nullptr);
    if (cache.mask_texture == nullptr) {
      overlay_gpu_free(cache);
      return;
    }
    /* A fresh texture starts undefined: clear it once here. From then on the incremental
     * updates below keep it in sync, the layout only ever changes through this branch. */
    /* Zero every layer through plain uploads: #GPU_texture_clear needs an attachment-capable
     * texture and clears a single layer of an array on the GL backend, so a texture created with
     * shader-read usage only would keep undefined texels and the shader would draw boundary
     * lines wherever that garbage crosses the 0.5 threshold. */
    constexpr int ZERO_ROWS_PER_UPLOAD = 256;
    const Array<float> zero_rows(int64_t(texture_size.x) * ZERO_ROWS_PER_UPLOAD, 0.0f);
    for (const int layer : cache.layer_tiles.index_range()) {
      for (int y = 0; y < texture_size.y; y += ZERO_ROWS_PER_UPLOAD) {
        GPU_texture_update_sub(cache.mask_texture,
                               GPU_DATA_FLOAT,
                               zero_rows.data(),
                               0,
                               y,
                               layer,
                               texture_size.x,
                               std::min(ZERO_ROWS_PER_UPLOAD, texture_size.y - y),
                               1);
      }
    }
    GPU_texture_extend_mode(cache.mask_texture, GPU_SAMPLER_EXTEND_MODE_EXTEND);
    GPU_texture_filter_mode(cache.mask_texture, true);
    cache.layer_uploaded.reinitialize(cache.layer_tiles.size());
    for (rcti &uploaded : cache.layer_uploaded) {
      BLI_rcti_init(&uploaded, 1, 0, 1, 0);
    }
  }
  /* On the reused-texture path the previously uploaded regions stay as they are: the update
   * below covers exactly where the mask may have changed. */
  overlay_mask_texture_update(cache, *image);

  overlay_tri_topology_update(ob, *image, cache);
  overlay_tri_batch_ensure(cache);
  if (cache.tris_batch == nullptr) {
    /* Nothing covers the selection (e.g. an empty boundary): keep the texture, nothing to draw.
     */
    return;
  }
  overlay_tri_positions_update(C, ob, cache);
}

void overlay_draw(const bContext *C, const ARegion &region)
{
  const Object *ob = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  if (ob == nullptr || scene == nullptr || scene->toolsettings == nullptr ||
      CTX_data_mode_enum(C) != CTX_MODE_SCULPT)
  {
    return;
  }
  OverlayCache &cache = overlay_cache_get();

  ImagePaintSelectionTarget primary = {};
  const bool has_target = image_paint_selection_view3d_primary_target_get(
      const_cast<bContext *>(C), *const_cast<Object *>(ob), primary);
  Image *image = has_target ? primary.image : nullptr;
  const bool has_selection = image != nullptr && BKE_image_paint_selection_mask_has_any(image);
  if (!has_selection) {
    if (cache.image != nullptr) {
      /* The last mask was removed: drop the cached geometry. */
      overlay_gpu_free(cache);
      cache = OverlayCache{};
    }
    return;
  }

  const uint64_t revision = BKE_image_paint_selection_mask_revision_get(image);
  const Mesh *mesh = id_cast<const Mesh *>(ob->data);
  const StringRef uv_name = BKE_paint_canvas_uvmap_name_get(
                                &scene->toolsettings->paint_mode, const_cast<Object *>(ob))
                                .value_or(mesh != nullptr ? mesh->active_or_default_uv_map_name() :
                                                            "");
  const bool stale = cache.image != image || cache.revision != revision ||
                     cache.ob_uid != uint32_t(ob->id.session_uid) ||
                     cache.corner_tri_num != (mesh ? int64_t(mesh->corner_tris().size()) : 0) ||
                     uv_name != cache.uv_map_name;
  if (stale) {
    /* Rebuild right away: the incremental path (bounds sub-updates into a reused texture, a
     * plain scan over cached triangle bounds) is cheap enough to run on every revision change,
     * so no redraw-gating is needed — region redraw tags would be ignored while drawing anyway. */
    overlay_rebuild(C, *ob, cache);
  }

  gpu::Shader *shader = overlay_shader_get();
  if (shader == nullptr || cache.tris_batch == nullptr || cache.mask_texture == nullptr) {
    return;
  }

  /* Follow the (deforming) evaluated mesh: re-upload the triangle positions when the PBVH
   * reports changed positions or the object transform moved; camera-only redraws (orbit, pan,
   * zoom) reuse the batch with no per-vertex CPU work. */
  const float4x4 object_to_world(ob->object_to_world());
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(*ob);
  const int64_t positions_count = pbvh != nullptr ? pbvh->positions_changed_count() : -1;
  if (positions_count != cache.positions_changed_count ||
      !(cache.object_to_world == object_to_world))
  {
    overlay_tri_positions_update(C, *ob, cache);
  }

  /* Save GPU state to restore after drawing. */
  const GPUBlend prev_blend = GPU_blend_get();
  const GPUDepthTest prev_depth_test = GPU_depth_test_get();

  GPU_blend(GPU_BLEND_ALPHA);
  /* The overlay is drawn on the surface itself: depth testing culls the parts hidden behind the
   * mesh with no per-vertex lift. */
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  /* The triangles lie exactly on the mesh surface, so without a depth offset they z-fight with it
   * and the dashed outline breaks up or vanishes; the offset pulls them just in front while the
   * depth test still hides the part behind the mesh. */
  const RegionView3D *rv3d = static_cast<const RegionView3D *>(region.regiondata);
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 1.0f);
  }
  /* POST_VIEW: the ambient matrix is the view-projection, so the batch's world-space vertices
   * map correctly without extra transforms. */
  GPU_batch_set_shader(cache.tris_batch, shader);
  GPU_texture_bind(cache.mask_texture, 0);
  GPU_batch_uniform_4f(cache.tris_batch, "line_color", 0.4f, 0.4f, 0.4f, 1.0f);
  GPU_batch_uniform_4f(cache.tris_batch, "line_color2", 1.0f, 1.0f, 1.0f, 1.0f);
  GPU_batch_uniform_1f(cache.tris_batch, "dash_width", SELECT_VIEW3D_DASH_WIDTH);
  GPU_batch_uniform_1f(cache.tris_batch, "fill_alpha", 0.0f);
  GPU_batch_draw(cache.tris_batch);
  GPU_texture_unbind(cache.mask_texture);

  /* Restore previous GPU state. */
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 0.0f);
  }
  GPU_depth_test(prev_depth_test);
  GPU_blend(prev_blend);
}

}  // namespace image_select_v3d

void image_select_view3d_region_draw(const bContext *C, ARegion *region, void * /*arg*/)
{
  if (C == nullptr || region == nullptr) {
    return;
  }
  image_select_v3d::overlay_draw(C, *region);
}

void image_select_view3d_overlay_free()
{
  image_select_v3d::OverlayCache &cache = image_select_v3d::overlay_cache_get();
  if (cache.mask_texture == nullptr && cache.tris_batch == nullptr) {
    /* Nothing allocated (no overlay drawn yet, e.g. in background mode): never touch the GPU
     * context machinery just to clear plain data. */
    cache = image_select_v3d::OverlayCache{};
    return;
  }
  /* Called from #view3d_free (file load, window close, exit): no GPU context is guaranteed to be
   * current here, and the discard calls below would assert or crash in the driver. Make the
   * shared viewport context current for the duration, unless the caller already holds it (a
   * nested enable would deadlock on its non-recursive mutex). */
  const bool context_enabled = DRW_gpu_context_is_enabled();
  if (!context_enabled) {
    DRW_gpu_context_enable();
  }
  image_select_v3d::overlay_gpu_free(cache);
  cache = image_select_v3d::OverlayCache{};
  if (!context_enabled) {
    /* Restores the window's drawable (the `_ex` variant without restore is only for closing
     * Blender): view3d_free also runs on file load and area changes, with windows still drawing. */
    DRW_gpu_context_disable();
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint

/* Public ED_ API (called from space_view3d) */
namespace blender {

void ED_paint_image_select_view3d_space_free(View3D *v3d)
{
  ed::sculpt_paint::image_select_view3d_space_free(v3d);
}

void ED_paint_image_select_view3d_draw_cb_register(ARegionType *art)
{
  ED_region_draw_cb_activate(
      art, ed::sculpt_paint::image_select_view3d_region_draw, nullptr, REGION_DRAW_POST_VIEW);
}

void ED_paint_image_select_view3d_overlay_free()
{
  ed::sculpt_paint::image_select_view3d_overlay_free();
}

}  // namespace blender

