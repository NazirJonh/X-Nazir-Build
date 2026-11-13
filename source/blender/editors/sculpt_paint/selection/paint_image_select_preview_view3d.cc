/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * See `paint_image_select_preview_view3d.hh`.
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_decal_log.hh"
#include "paint_image_select_editor_decal.hh"
#include "paint_image_select_preview_view3d.hh"
#include "paint_image_select_preview_view3d_intern.hh"
#include "paint_image_select_surface_decal.hh"
#include "paint_image_select_view3d.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Preview
 * \{ */

ImageSelect3DPreview::~ImageSelect3DPreview()
{
  for (gpu::Texture *&tex : this->stamp_textures_2d) {
    image_select_view3d_preview_texture_free(tex);
  }
  surface_decal_release(this->decal.session);
  image_select_view3d_preview_textures_free(this->textures);
  for (gpu::Texture *&tex : this->pbr_default_textures) {
    image_select_view3d_preview_texture_free(tex);
  }
}

/** True when the active object's canvas maps include one of the session's images. */
static bool preview_object_uses_canvas(bContext *C,
                                       Object &ob,
                                       const ImageSelect3DPreview &preview)
{
  for (const ImagePaintSelectionTarget &candidate : image_paint_selection_view3d_targets_get(C, ob))
  {
    for (const ImagePaintSelectionTarget &mine : preview.targets) {
      if (mine.image == candidate.image) {
        return true;
      }
    }
  }
  return false;
}

/** The UV affine preview, the on-surface counterpart of the 3D tool's #transform3d_draw_texture. */
static void preview_draw_uv(ImageSelect3DPreview &preview, const ARegion *region, const Object &ob)
{
  const ImageSelectEditorMapping &mapping = preview.frame.mapping;
  const Span<Vector<SelectionTileFragment>> fragments = preview.frame.fragments;
  if (preview.textures.size() != fragments.size()) {
    preview.textures.resize(fragments.size());
  }
  const int target_i = image_select_preview_target_resolve(preview.targets, -1);
  if (target_i >= fragments.size()) {
    return;
  }
  bool invert_ok = false;
  const float2x2 linear_inv = math::invert(mapping.linear, invert_ok);
  if (!invert_ok) {
    return;
  }
  const float2x2 linear = mapping.linear;
  const float2 pivot = mapping.pivot;
  const float2 translation = mapping.translation;
  const auto forward = [&](const float2 &src_uv) {
    return pivot + linear * (src_uv - pivot) + translation;
  };
  const auto src_uv_of = [&](const float2 &dest_uv) {
    return pivot + linear_inv * (dest_uv - pivot - translation);
  };
  /* The preview triangles lie exactly on the mesh surface: without a depth offset the shared
   * pixels z-fight and the texture shows through only in patches. */
  const RegionView3D *rv3d = (region != nullptr) ?
                                 static_cast<const RegionView3D *>(region->regiondata) :
                                 nullptr;
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 1.0f);
  }
  if (target_i < 0) {
    image_select_view3d_draw_pbr_composite(ob,
                                           preview.surface_map,
                                           region,
                                           preview.targets,
                                           fragments,
                                           preview.textures,
                                           preview.pbr_default_textures,
                                           forward,
                                           src_uv_of);
  }
  else {
    const Vector<SelectionTileFragment> &per_target = fragments[target_i];
    for (const int frag_i : per_target.index_range()) {
      const SelectionTileFragment &frag = per_target[frag_i];
      gpu::Texture *texture = image_select_view3d_fragment_texture(
          preview.textures, fragments, target_i, frag_i);
      if (texture == nullptr) {
        continue;
      }
      const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
      const float2 tile_size = float2(frag.geom.tile_size_px);
      const float2 src_origin_uv = tile_uv + float2(frag.geom.origin_px) / tile_size;
      const float2 src_size_uv = float2(frag.geom.size_px) / tile_size;
      /* Destination rect of this fragment: its four source corners through the forward map. */
      float2 dest_min = float2(FLT_MAX, FLT_MAX);
      float2 dest_max = float2(-FLT_MAX, -FLT_MAX);
      for (const int i : IndexRange(4)) {
        const float2 corner_src = src_origin_uv + float2((i & 1) ? src_size_uv.x : 0.0f,
                                                         (i & 2) ? src_size_uv.y : 0.0f);
        const float2 corner_dest = forward(corner_src);
        dest_min = math::min(dest_min, corner_dest);
        dest_max = math::max(dest_max, corner_dest);
      }
      image_select_view3d_draw_uv_texture_ext(
          ob,
          preview.surface_map,
          texture,
          dest_min,
          dest_max,
          [&](const float2 &dest_uv) { return (src_uv_of(dest_uv) - src_origin_uv) / src_size_uv; });
    }
  }
  if (rv3d != nullptr) {
    ED_view3d_polygon_offset(rv3d, 0.0f);
  }
}

static void preview_draw(const bContext *C, ARegion *region, void *arg)
{
  ImageSelect3DPreview *preview = static_cast<ImageSelect3DPreview *>(arg);
  if (preview == nullptr || preview->session == nullptr || preview->frame_get == nullptr ||
      region == nullptr || C == nullptr)
  {
    return;
  }
  if (region->regiontype != RGN_TYPE_WINDOW || CTX_wm_view3d(C) == nullptr) {
    return;
  }
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH || ob->data == nullptr) {
    return;
  }
  /* No object with this canvas in this Viewport: nothing to draw, nothing to report. */
  if (!preview_object_uses_canvas(const_cast<bContext *>(C), *ob, *preview)) {
    return;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob->data);
  /* The resolved name: an empty one never matches the map's stored name and rebuilds every call. */
  if (!preview->surface_map.ensure(*ob, mesh.active_or_default_uv_map_name())) {
    return;
  }
  if (!preview->frame_get(*preview->session, preview->targets, preview->frame)) {
    return;
  }
  if (preview->frame.fragments.size() != preview->targets.size()) {
    return;
  }

  if (!preview->decal_mode) {
    preview_draw_uv(*preview, region, *ob);
    return;
  }
  if (preview->decal.object != ob) {
    return;
  }
  /* Keeps the last valid patch when the fragment is dragged off the surface. */
  image_select_editor_decal_update(
      C, *ob, preview->surface_map, preview->decal, preview->frame.mapping);
  surface_decal_draw_preview(region,
                             preview->decal.session,
                             *ob,
                             preview->surface_map,
                             preview->targets,
                             preview->textures,
                             preview->pbr_default_textures,
                             image_select_preview_target_resolve(preview->targets, -1));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

Vector<ImagePaintSelectionTarget> image_select_editor_targets_get(
    const bContext *C, const SpaceImage *sima, const bool primary_target_only)
{
  Vector<ImagePaintSelectionTarget> targets = image_paint_selection_targets_get(C, sima);
  if (primary_target_only) {
    Vector<ImagePaintSelectionTarget> primary;
    for (const ImagePaintSelectionTarget &target : targets) {
      if (target.image == sima->image) {
        primary.append(target);
        break;
      }
    }
    return primary;
  }
  return targets;
}

bool image_select_editor_decal_requested(const bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  return scene != nullptr && scene->toolsettings != nullptr &&
         scene->toolsettings->imapaint.selection_transform_mode ==
             IMAGE_PAINT_SELECT_TRANSFORM_DECAL;
}

bool image_select_preview_view3d_attach(bContext *C,
                                        PaintSelectFloatingSession &session,
                                        const ImageSelect3DPreviewFrameFn frame_get,
                                        const bool primary_target_only,
                                        const bool want_decal,
                                        ReportList *reports)
{
  image_select_preview_view3d_detach(session);
  SpaceImage *sima = session.owner_sima;
  /* A new session makes the decal a previous confirm left behind obsolete. */
  image_select_editor_decal_handoff_release(sima);
  if (sima == nullptr || sima->image == nullptr || frame_get == nullptr) {
    return false;
  }
  ARegionType *region_type = BKE_regiontype_from_id(BKE_spacetype_from_id(SPACE_VIEW3D),
                                                    RGN_TYPE_WINDOW);
  if (region_type == nullptr) {
    return false;
  }
  Vector<ImagePaintSelectionTarget> targets = image_select_editor_targets_get(
      C, sima, primary_target_only);
  if (targets.is_empty()) {
    return false;
  }

  auto *preview = MEM_new<ImageSelect3DPreview>(__func__);
  preview->session = &session;
  preview->region_type = region_type;
  preview->targets = std::move(targets);
  preview->frame_get = frame_get;
  preview->primary_target_only = primary_target_only;

  if (want_decal) {
    /* The decal needs the active mesh object, its UV map and a surface under the fragment; any
     * miss keeps the UV mapping (the session then commits through the affine as before). */
    Object *ob = CTX_data_active_object(C);
    const Mesh *mesh = (ob != nullptr && ob->type == OB_MESH) ? id_cast<const Mesh *>(ob->data) :
                                                               nullptr;
    if (mesh != nullptr && frame_get(session, preview->targets, preview->frame) &&
        preview->frame.fragments.size() == preview->targets.size() &&
        preview->surface_map.ensure(*ob, mesh->active_or_default_uv_map_name()) &&
        image_select_editor_decal_begin(C,
                                        *ob,
                                        preview->surface_map,
                                        preview->targets,
                                        preview->frame.fragments,
                                        preview->decal))
    {
      preview->decal_mode = true;
    }
    else {
      BKE_report(reports, RPT_WARNING, "Decal: no surface for the selection; transforming in UV mode");
    }
  }

  preview->handle = ED_region_draw_cb_activate(
      region_type, preview_draw, preview, REGION_DRAW_POST_VIEW);
  session.preview_3d = preview;
  return preview->decal_mode;
}

bool image_select_preview_view3d_mode_switch(bContext *C,
                                             PaintSelectFloatingSession &session,
                                             const bool want_decal,
                                             bool &r_decal_active)
{
  const ImageSelect3DPreview *preview = session.preview_3d;
  if (preview == nullptr) {
    return false;
  }
  r_decal_active = preview->decal_mode;
  if (preview->decal_mode == want_decal) {
    return true;
  }
  /* The gesture (mapping) lives in the session, so rebuilding the preview loses nothing: attach
   * frees the old preview (callback, GPU textures, decal stamps) before it builds the new one. The
   * attach arguments are read first because the detach inside it deletes the preview. */
  const ImageSelect3DPreviewFrameFn frame_get = preview->frame_get;
  const bool primary_target_only = preview->primary_target_only;
  r_decal_active = image_select_preview_view3d_attach(
      C, session, frame_get, primary_target_only, want_decal, nullptr);
  if (session.preview_3d == nullptr) {
    /* The attach refused without a preview (no image / targets / region type). */
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                           "live mode switch: preview could not be rebuilt\n");
    return false;
  }
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_EDITOR
                         "live mode switch: requested %s, now %s\n",
                         want_decal ? "Decal" : "UV",
                         r_decal_active ? "Decal" : "UV");
  return true;
}

void image_select_preview_view3d_detach(PaintSelectFloatingSession &session)
{
  ImageSelect3DPreview *preview = session.preview_3d;
  if (preview == nullptr) {
    return;
  }
  session.preview_3d = nullptr;
  if (preview->handle != nullptr && preview->region_type != nullptr) {
    ED_region_draw_cb_exit(preview->region_type, preview->handle);
  }
  preview->handle = nullptr;
  MEM_delete(preview);
}

void image_select_preview_view3d_tag_redraw(const bContext *C,
                                            const PaintSelectFloatingSession &session)
{
  if (session.preview_3d == nullptr || C == nullptr) {
    return;
  }
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm == nullptr) {
    return;
  }
  for (wmWindow &win : wm->windows) {
    bScreen *screen = WM_window_get_active_screen(&win);
    if (screen == nullptr) {
      continue;
    }
    for (ScrArea &area : screen->areabase) {
      if (area.spacetype != SPACE_VIEW3D) {
        continue;
      }
      for (ARegion &region : area.regionbase) {
        if (region.regiontype == RGN_TYPE_WINDOW) {
          ED_region_tag_redraw(&region);
        }
      }
    }
  }
}

/** \} */

}  // namespace blender::ed::sculpt_paint
