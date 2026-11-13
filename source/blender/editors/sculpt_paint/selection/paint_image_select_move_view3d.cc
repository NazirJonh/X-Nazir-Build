/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Move Selection in the 3D Viewport: lift the selected pixels into floating fragments, drag them
 * on the anchor plane (the screen drag maps to a UV offset through the anchor's plane->UV
 * Jacobian), preview the outline on the surface, and commit with a single image undo step per
 * canvas. The fragment data model and the commit resampling are shared with the Image Editor's
 * move tool; only the input mapping and the preview differ. Copy/paste build on the same
 * fragments.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_library.hh"
#include "BKE_screen.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_image.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "paint_image_select_floating.hh"
#include "GEO_reverse_uv_sampler.hh"
#include "paint_image_select_decal_log.hh"
#include "paint_image_select_editor_decal.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_move_intern.hh"
#include "paint_image_select_paste_layer.hh"
#include "paint_image_select_surface_decal.hh"
#include "paint_image_select_view3d.hh"
#include "../mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

namespace image_select_v3d {

/* The commit write-back is the shared affine fragment writer
 * (#image_select_view3d_fragments_write_final) with the identity linear part: see
 * paint_image_select_view3d.cc. */

/* The copy/paste clipboard is the shared 2D/3D #ImageSelectClipboard (see
 * paint_image_select_fragment.hh): a Copy in the 3D Viewport pastes in the Image Editor and vice
 * versa. Fragments carry no image reference, so the owning images are recorded per group by
 * session UID and re-resolved on paste. */

}  // namespace image_select_v3d

struct ImageSelectMove3DState : PaintSelectView3DFloatingSession {
  ImageSelectView3DAnchor anchor;
  /** The canvas images the fragments were lifted from, parallel to #fragments. */
  Vector<ImagePaintSelectionTarget> targets;
  /** One fragment list per target. */
  Vector<Vector<SelectionTileFragment>> fragments;
  /** Accumulated drag offset, global UV. */
  float2 uv_drag_offset = float2(0.0f);
  /** Previous mouse position (region coords) of the active drag, for plane-delta mapping. */
  int2 prev_mouse_xy = int2(0);
  /** Snapshot of #uv_drag_offset per finished gesture, for step-back. */
  Vector<float2> drag_offset_history;
  /** Fragment outlines in global UV at lift time (per target, segment pairs). */
  Vector<Vector<float2>> outline_uv;
  ImageSelectView3DSurfaceMap surface_map;
  /** GPU copies of the fragments' display buffers for the on-surface preview, created lazily by
   * the draw callback (a GPU context is current there), parallel to #fragments. */
  Vector<Vector<gpu::Texture *>> preview_textures;
  /**
   * Which target the on-surface preview shows; -1 = the PBR composite (a Material canvas's maps
   * shaded together through #GPU_SHADER_IMAGE_SELECT_PBR), or the auto target when no composite
   * is possible. Cycled by #preview_channel_cycle between the composite and the channels it
   * cannot shade (a Material canvas lifts one fragment set per PBR map; stacking them flat reads
   * as nothing, which is why the composite is the default now).
   */
  int preview_target = -1;
  /** Neutral 1x1 stand-ins of the PBR composite, one per #PBRPreviewChannel. */
  gpu::Texture *pbr_default_textures[PBR_PREVIEW_CHANNEL_NUM] = {nullptr};
  /**
   * The content mapping of this session: #use_decal carries the fragment over the surface under
   * the cursor (the Image Editor's decal frame, authored from #uv_drag_offset, which stays the one
   * gesture state in both modes so the mode switches without loss); false is the UV offset. Read
   * from the tool settings at invoke, switched live by #move3d_mode_switch.
   */
  bool use_decal = false;
  ImageSelectEditorDecal decal;
  /** Set when #uv_drag_offset changed outside a drag step (a step-back): the next draw callback,
   * which always has a context, refreshes the decal patch. */
  bool decal_dirty = false;
  /** Decal drag: the cursor's last surface point (object space) for the delta that does not jump at
   * UV seams, and the press offset of the center from the cursor, for the re-seat across a seam. */
  float3 drag_prev_co = float3(0.0f);
  bool drag_prev_valid = false;
  float2 drag_grab_uv = float2(0.0f);
  float drag_grab_len = 0.0f;

  explicit ImageSelectMove3DState()
      : PaintSelectView3DFloatingSession(PaintSelectView3DTool::Move)
  {
  }

  /** See #image_select_preview_target_resolve. */
  int preview_target_resolved() const
  {
    return image_select_preview_target_resolve(this->targets, this->preview_target);
  }

  void preview_channel_cycle() override
  {
    this->preview_target = image_select_preview_target_cycle(this->targets, this->preview_target);
  }

  /** Free the preview textures; makes the GPU context current when the caller has none (a
   * Viewport freed while a session floats, file load, exit). */
  void preview_textures_free()
  {
    image_select_view3d_preview_textures_free(this->preview_textures);
    for (gpu::Texture *&tex : this->pbr_default_textures) {
      image_select_view3d_preview_texture_free(tex);
    }
  }

  ~ImageSelectMove3DState() override
  {
    surface_decal_release(this->decal.session);
    this->preview_textures_free();
    for (Vector<SelectionTileFragment> &per_target : this->fragments) {
      selection_tile_fragments_free(per_target);
    }
    this->fragments.clear();
  }

  void commit(bContext *C) override;
  void cancel(bContext *C) override;
  bool undo_step() override;
  bool cursor_over_fragment(const bContext *C, const int2 &mval) const override;
};

/** Remove the preview draw callback (safe to call twice). */
static void move3d_state_free_draw_handle(ImageSelectMove3DState &state)
{
  image_select_view3d_floating_draw_handle_clear(state);
}

/* -------------------------------------------------------------------- */
/** \name Decal mode
 *
 * The fragment rides the surface under the cursor instead of the UV offset: the decal frame is the
 * Image Editor's (#ImageSelectEditorDecal, authored from #ImageSelectMove3DState::uv_drag_offset as
 * the translation of the source cage center, no rotation, scale 1), so the orientation is the
 * surface direction of +U at the center and the fragment never turns with the gesture. The preview
 * and the bake are the shared surface-decal core's.
 * \{ */

/** Whether the tool settings ask for the Decal mapping. */
static bool move3d_decal_requested(const bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  return scene != nullptr && scene->toolsettings != nullptr &&
         scene->toolsettings->imapaint.selection_transform_mode ==
             IMAGE_PAINT_SELECT_TRANSFORM_DECAL;
}

/** Bring the decal frame and its patch to the current #ImageSelectMove3DState::uv_drag_offset. */
static void move3d_decal_update(const bContext *C, ImageSelectMove3DState &state)
{
  if (state.anchor.object == nullptr) {
    return;
  }
  ImageSelectEditorMapping mapping;
  mapping.translation = state.uv_drag_offset;
  image_select_editor_decal_update(C, *state.anchor.object, state.surface_map, state.decal, mapping);
  state.decal_dirty = false;
}

/**
 * Open the decal half of the session at the current offset. False (the session stays in UV mode)
 * when the object is no mesh or no stamp or surface patch could be built.
 */
static bool move3d_decal_begin(const bContext *C, ImageSelectMove3DState &state)
{
  Object *ob = state.anchor.object;
  if (ob == nullptr || ob->type != OB_MESH || state.surface_map.sampler == nullptr) {
    return false;
  }
  ImageSelectEditorDecal decal;
  if (!image_select_editor_decal_begin(C, *ob, state.surface_map, state.targets, state.fragments, decal))
  {
    return false;
  }
  surface_decal_release(state.decal.session);
  state.decal = std::move(decal);
  state.use_decal = true;
  state.drag_prev_valid = false;
  move3d_decal_update(C, state);
  return true;
}

/** True when \a uv samples onto the surface within \a tolerance (object units) of \a expected_co. */
static bool move3d_decal_uv_near(const bContext *C,
                                 const ImageSelectMove3DState &state,
                                 const float2 &uv,
                                 const float3 &expected_co,
                                 const float tolerance)
{
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr || state.anchor.object == nullptr) {
    return false;
  }
  SurfaceDecalFrame frame;
  if (!surface_decal_frame_at_uv(
          *state.anchor.object, *depsgraph, state.surface_map, uv, state.decal.tangent, frame))
  {
    return false;
  }
  return math::distance(frame.co, expected_co) <= tolerance;
}

/** Capture the cursor's surface point and its offset from the decal center at a press. */
static void move3d_decal_drag_begin(bContext *C,
                                    ImageSelectMove3DState &state,
                                    const ARegion &region,
                                    const int2 &mval)
{
  state.drag_prev_valid = false;
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  float2 uv;
  float3 co;
  if (depsgraph == nullptr || state.anchor.object == nullptr ||
      !image_select_view3d_surface_point_at(C, *state.anchor.object, state.anchor, region, mval, uv, co))
  {
    return;
  }
  const float2 center_uv = state.decal.source_center_uv + state.uv_drag_offset;
  state.drag_grab_uv = center_uv - uv;
  state.drag_grab_len = 0.0f;
  SurfaceDecalFrame frame;
  if (surface_decal_frame_at_uv(
          *state.anchor.object, *depsgraph, state.surface_map, center_uv, state.decal.tangent, frame))
  {
    state.drag_grab_len = math::distance(frame.co, co);
  }
  state.drag_prev_co = co;
  state.drag_prev_valid = true;
}

/**
 * One drag step in Decal mode: the center follows the surface point under the cursor. Its
 * object-space delta is projected onto the center's frame and pushed through that frame's
 * plane -> UV Jacobian, so crossing an edge inside an island keeps the fragment glued to the
 * surface; past a seam or a gap (the Jacobian lands off the cursor's point) the center is re-seated
 * from the cursor's own UV, shifted back by the press offset when that lands on the surface too. A
 * center without a surface under it is never taken: the fragment stays where it last was.
 */
static void move3d_decal_drag_step(bContext *C,
                                   ImageSelectMove3DState &state,
                                   const ARegion &region,
                                   const int2 &mval)
{
  const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (depsgraph == nullptr || state.anchor.object == nullptr) {
    return;
  }
  Object &ob = *state.anchor.object;
  float2 uv_now;
  float3 co_now;
  if (!image_select_view3d_surface_point_at(C, ob, state.anchor, region, mval, uv_now, co_now)) {
    return;
  }
  if (!state.drag_prev_valid) {
    move3d_decal_drag_begin(C, state, region, mval);
    return;
  }
  const float2 center_uv = state.decal.source_center_uv + state.uv_drag_offset;
  const float3 d3 = co_now - state.drag_prev_co;
  float2 new_center = uv_now;
  bool found = false;
  SurfaceDecalFrame frame;
  const bool frame_ok = surface_decal_frame_at_uv(
      ob, *depsgraph, state.surface_map, center_uv, state.decal.tangent, frame);
  if (frame_ok && frame.jacobian_valid) {
    const float2 d_plane = float2(math::dot(d3, frame.tangent), math::dot(d3, frame.bitangent));
    const float2 advanced = center_uv + frame.plane_uv_jacobian * d_plane;
    if (move3d_decal_uv_near(C, state, advanced, frame.co + d3, 0.6f * math::length(d3) + 1e-5f)) {
      new_center = advanced;
      found = true;
    }
  }
  if (!found) {
    const float2 shifted = uv_now + state.drag_grab_uv;
    new_center = move3d_decal_uv_near(C, state, shifted, co_now, state.drag_grab_len * 1.5f + 1e-5f) ?
                     shifted :
                     uv_now;
  }
  state.drag_prev_co = co_now;
  if (frame_ok) {
    /* Kept continuous across the drag: the stamp is re-oriented by +U on the surface at the new
     * center, the previous tangent is only the fallback where +U cannot be sampled. */
    state.decal.tangent = frame.tangent;
  }
  if (!move3d_decal_uv_near(C, state, new_center, co_now, std::numeric_limits<float>::max())) {
    /* No surface under the new center (a UV gap): the fragment stays on its last valid point. */
    return;
  }
  state.uv_drag_offset = new_center - state.decal.source_center_uv;
  move3d_decal_update(C, state);
}

/**
 * Switch the live session between the UV offset and the Decal mapping. The gesture is the UV drag
 * offset in both modes, so nothing is lost: Decal -> UV keeps the offset (the UV the decal center
 * stands at), UV -> Decal opens the decal frame with its center at the fragment's current center.
 * A running drag defers the switch (see #image_select_view3d_pending_mode_apply); the mode stays
 * when the decal cannot be opened.
 * \return true when the mode changed.
 */
static bool move3d_mode_switch(bContext *C, ImageSelectMove3DState &state, const bool want_decal)
{
  if (state.use_decal == want_decal) {
    /* The setting went back to the session's mode: a change kept during the gesture is void. */
    state.pending_decal = -1;
    return false;
  }
  if (state.is_dragging) {
    state.pending_decal = want_decal ? 1 : 0;
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                           "move: mode change deferred until the drag ends\n");
    return false;
  }
  state.pending_decal = -1;
  /* The preview table holds stamp copies in Decal mode and fragment copies in UV mode, in the same
   * slots: drop it so neither mode draws the other's textures. */
  image_select_view3d_preview_textures_free(state.preview_textures);
  if (!want_decal) {
    surface_decal_release(state.decal.session);
    state.decal = ImageSelectEditorDecal();
    state.use_decal = false;
    state.decal_dirty = false;
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D "move: live switch Decal -> UV\n");
    return true;
  }
  if (!move3d_decal_begin(C, state)) {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                           "move: live switch to Decal refused: no mesh object, stamp or "
                           "surface patch\n");
    return false;
  }
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                         "move: live switch UV -> Decal at offset (%.4f, %.4f)\n",
                         state.uv_drag_offset.x,
                         state.uv_drag_offset.y);
  return true;
}

/** \} */

/** Boundary segments of one fragment's mask in global UV, at its lifted position. */
static Vector<float2> move3d_fragment_outlines_uv(const SelectionTileFragment &frag)
{
  Vector<float2> segments;
  if (frag.pixels.fragment_mask_ibuf == nullptr) {
    return segments;
  }
  const ImBuf *mask = frag.pixels.fragment_mask_ibuf;
  const float *data = mask->float_data();
  if (data == nullptr) {
    return segments;
  }
  const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
  const float2 origin = float2(frag.geom.origin_px);
  const float2 tile_size = float2(frag.geom.tile_size_px);
  const auto uv_cell = [&](const int2 &px) {
    return tile_uv + (origin + float2(px)) / tile_size;
  };
  image_select_mask_boundary_segments(
      data, int2(mask->x, mask->y), int2(0, 0), int2(mask->x, mask->y), uv_cell, 1, segments);
  return segments;
}

/** The 3D preview: every fragment outline (translated by the drag offset) on the surface. */
static void move3d_draw(const bContext *C, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectMove3DState *>(arg);
  if (state == nullptr || state->anchor.object == nullptr || !state->surface_map.sampler) {
    return;
  }
  if (state->use_decal) {
    /* A step-back moved the offset outside a drag step: this callback is the one caller that always
     * has a context, so the patch is brought up to date here. */
    if (state->decal_dirty && C != nullptr) {
      move3d_decal_update(C, *state);
    }
    /* The patch carries the stamp at the real surface positions; the UV-space path below does not
     * apply (and the yellow outline would sit where the fragment was lifted, not where it is). */
    surface_decal_draw_preview(region,
                               state->decal.session,
                               *state->anchor.object,
                               state->surface_map,
                               state->targets,
                               state->preview_textures,
                               state->pbr_default_textures,
                               state->preview_target_resolved());
    return;
  }
  /* The fragment pixels at their dragged position, on the surface; the source region is already
   * cleared on the canvas, so this is the only place the content shows while it floats. */
  if (state->preview_textures.size() != state->fragments.size()) {
    state->preview_textures.resize(state->fragments.size());
  }
  /* A Material canvas lifts one fragment set per PBR map, all covering the same UV rect: the
   * composite shades them together into one material patch. The channels it cannot shade (AO and
   * the rest) stay flat single maps, cycled by Q. */
  const int preview_target = state->preview_target_resolved();
  /* The preview triangles lie exactly on the mesh surface: without a depth offset the shared
   * pixels z-fight and the texture shows through only in patches. */
  if (region != nullptr && region->regiondata != nullptr) {
    ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 1.0f);
  }
  if (preview_target < 0) {
    const float2 drag = state->uv_drag_offset;
    image_select_view3d_draw_pbr_composite(
        *state->anchor.object,
        state->surface_map,
        region,
        state->targets,
        state->fragments,
        state->preview_textures,
        state->pbr_default_textures,
        [&](const float2 &src_uv) { return src_uv + drag; },
        [&](const float2 &dest_uv) { return dest_uv - drag; });
  }
  else if (preview_target < state->fragments.size()) {
    const Vector<SelectionTileFragment> &per_target = state->fragments[preview_target];
    Vector<gpu::Texture *> &textures = state->preview_textures[preview_target];
    if (textures.size() != per_target.size()) {
      textures.resize(per_target.size(), nullptr);
    }
    for (const int frag_i : per_target.index_range()) {
      const SelectionTileFragment &frag = per_target[frag_i];
      if (frag.preview.fragment_display_ibuf == nullptr) {
        continue;
      }
      gpu::Texture *texture = image_select_view3d_fragment_texture(
          state->preview_textures, state->fragments, preview_target, frag_i);
      if (texture == nullptr) {
        continue;
      }
      const float2 tile_size = float2(frag.geom.tile_size_px);
      const float2 uv_min = image_select_udim_tile_uv_origin(frag.geom.tile_number) +
                            float2(frag.geom.origin_px) / tile_size + state->uv_drag_offset;
      image_select_view3d_draw_uv_texture(*state->anchor.object,
                                          state->surface_map,
                                          texture,
                                          uv_min,
                                          float2(frag.geom.size_px) / tile_size);
    }
  }
  if (region != nullptr && region->regiondata != nullptr) {
    ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 0.0f);
  }

  Vector<Vector<float2>> polylines;
  for (const Vector<float2> &outline : state->outline_uv) {
    if (outline.is_empty()) {
      continue;
    }
    Vector<float2> translated;
    translated.resize(outline.size());
    for (const int i : outline.index_range()) {
      translated[i] = outline[i] + state->uv_drag_offset;
    }
    polylines.append(std::move(translated));
  }
  constexpr float yellow[4] = {1.0f, 0.85f, 0.0f, 0.9f};
  image_select_view3d_draw_uv_outlines(
      *state->anchor.object, state->surface_map, polylines, yellow, 8.0f);
}

/**
 * Bake the stamps through the decal patch at the final offset: one image undo step, the source
 * regions cut like the UV commit's. False, with nothing written, when no patch can be built; the
 * caller then commits the UV offset, which is the same gesture.
 */
static bool move3d_decal_commit(bContext *C, ImageSelectMove3DState &state)
{
  if (state.anchor.object == nullptr) {
    return false;
  }
  move3d_decal_update(C, state);
  if (state.decal.session.patch.tris.is_empty()) {
    IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                           "move: commit has no surface patch, committing the UV offset\n");
    return false;
  }
  surface_decal_commit(C,
                       "Move Selection (Decal)",
                       state.decal.session,
                       state.surface_map,
                       state.targets,
                       state.fragments,
                       [&](const Image *ima) { return image_select_view3d_image_is_alive(state, ima); });
  IMAGE_SELECT_DECAL_LOG(IMAGE_SELECT_DECAL_TAG_3D
                         "move: commit bake: %d target(s) through %d patch tris\n",
                         int(state.targets.size()),
                         int(state.decal.session.patch.tris.size()));
  surface_decal_release(state.decal.session);
  return true;
}

void ImageSelectMove3DState::commit(bContext *C)
{
  move3d_state_free_draw_handle(*this);
  WM_cursor_modal_restore(CTX_wm_window(C));
  if (this->use_decal && move3d_decal_commit(C, *this)) {
    return;
  }

  /* The committable targets, in resolve order: the whole commit lands in ONE image undo step, so
   * a single Ctrl+Z restores every Material canvas together (see
   * #image_select_view3d_commit_targets_with_undo). */
  Vector<ImagePaintSelectionTarget> commit_targets;
  Vector<Span<SelectionTileFragment>> commit_fragments;
  for (const int target_i : this->targets.index_range()) {
    Image *ima = this->targets[target_i].image;
    if (ima == nullptr || this->fragments[target_i].is_empty() ||
        !image_select_view3d_image_is_alive(*this, ima))
    {
      /* The image was freed / re-created by a file load or undo while the fragment floated:
       * there is no canvas to write back to. */
      continue;
    }
    commit_targets.append(this->targets[target_i]);
    commit_fragments.append(Span<SelectionTileFragment>(this->fragments[target_i]));
  }
  const float2 offset = this->uv_drag_offset;
  image_select_view3d_commit_targets_with_undo(
      C,
      "Move Selection",
      commit_targets,
      commit_fragments,
      [&](Image &ima, const ImageUser &iuser, Span<SelectionTileFragment> fragments) {
        image_select_view3d_fragments_write_final(C,
                                                  &ima,
                                                  iuser,
                                                  fragments,
                                                  float2x2::identity(),
                                                  float2(0.0f),
                                                  offset);
      });
}

void ImageSelectMove3DState::cancel(bContext *C)
{
  move3d_state_free_draw_handle(*this);
  if (C != nullptr) {
    WM_cursor_modal_restore(CTX_wm_window(C));
  }
  for (const int target_i : this->targets.index_range()) {
    Image *ima = this->targets[target_i].image;
    if (ima == nullptr || !image_select_view3d_image_is_alive(*this, ima)) {
      continue;
    }
    image_select_fragment_restore_source(
        C, ima, this->targets[target_i].iuser, Span<SelectionTileFragment>(this->fragments[target_i]));
    Vector<ImagePaintSelectionTarget> single = {this->targets[target_i]};
    image_paint_selection_targets_update(C, single);
  }
}

bool ImageSelectMove3DState::undo_step()
{
  if (this->drag_offset_history.is_empty()) {
    return false;
  }
  this->uv_drag_offset = this->drag_offset_history.pop_last();
  this->decal_dirty = true;
  if (this->owner_region != nullptr) {
    ED_region_tag_redraw(this->owner_region);
  }
  return true;
}

bool ImageSelectMove3DState::cursor_over_fragment(const bContext *C, const int2 &mval) const
{
  ARegion *region = CTX_wm_region(C);
  if (region != nullptr && this->use_decal && this->anchor.object != nullptr) {
    /* The decal rides the surface, which the UV rect below cannot follow across a seam: the cursor
     * grabs it within the circle around the stamp that the stamp's own extents span. */
    const Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    float2 uv;
    float3 co;
    SurfaceDecalFrame frame;
    if (depsgraph == nullptr ||
        !image_select_view3d_surface_point_at(const_cast<bContext *>(C),
                                              *this->anchor.object,
                                              this->anchor,
                                              *region,
                                              mval,
                                              uv,
                                              co) ||
        !surface_decal_frame_at_uv(*this->anchor.object,
                                   *depsgraph,
                                   this->surface_map,
                                   this->decal.source_center_uv + this->uv_drag_offset,
                                   this->decal.tangent,
                                   frame))
    {
      return false;
    }
    return math::distance(co, frame.co) <= 0.5f * math::length(this->decal.session.base_size);
  }
  if (region == nullptr || !this->anchor.jacobian_valid || this->anchor.object == nullptr) {
    return false;
  }
  float2 plane_px;
  if (!image_select_view3d_mval_to_plane_px(this->anchor, *region, mval, plane_px)) {
    return false;
  }
  /* The preview is drawn through the surface map (UV -> surface -> screen), so the hit test must
   * resolve the cursor the same way: the anchor jacobian is only exact at the anchor triangle, and
   * a fragment dragged across a curved or distorted surface sits under the cursor while a
   * jacobian-based test would miss it (the Image Editor's test reads the cursor through the same
   * view2d mapping the preview uses). */
  float2 mouse_uv;
  if (!image_select_view3d_plane_px_to_surface_uv(const_cast<bContext *>(C),
                                                  this->anchor,
                                                  plane_px,
                                                  mouse_uv))
  {
    /* Off-surface (e.g. the plane leaves the mesh): keep the cheap jacobian estimate so a press
     * inside the fragment still starts the drag. */
    mouse_uv = this->anchor.anchor_uv +
               image_select_view3d_plane_px_to_uv_delta(this->anchor, plane_px);
  }
  for (const Vector<SelectionTileFragment> &per_target : this->fragments) {
    for (const SelectionTileFragment &frag : per_target) {
      /* The user-visible rect (tight selection box), like the Image Editor's hit test: the
       * feathered footprint beyond it must not grab a press meant for the empty area. */
      const int2 ui_origin = image_select_fragment_ui_origin(frag);
      const int2 ui_size = image_select_fragment_ui_size(frag);
      const float2 tile_uv = image_select_udim_tile_uv_origin(frag.geom.tile_number);
      const float2 lo = tile_uv + float2(ui_origin) / float2(frag.geom.tile_size_px) +
                        this->uv_drag_offset;
      const float2 hi = lo + float2(ui_size) / float2(frag.geom.tile_size_px);
      if (mouse_uv.x >= lo.x && mouse_uv.x <= hi.x && mouse_uv.y >= lo.y && mouse_uv.y <= hi.y) {
        return true;
      }
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

static ImageSelectMove3DState *move3d_state_get()
{
  return image_select_view3d_session_get<ImageSelectMove3DState, PaintSelectView3DTool::Move>();
}

void image_select_move3d_mode_changed(bContext *C, const bool want_decal)
{
  ImageSelectMove3DState *state = move3d_state_get();
  if (state == nullptr || C == nullptr) {
    return;
  }
  if (move3d_mode_switch(C, *state, want_decal) && state->owner_region != nullptr) {
    ED_region_tag_redraw(state->owner_region);
  }
}

static bool move3d_lift_fragments(bContext *C,
                                  wmOperator *op,
                                  ImageSelectMove3DState &state,
                                  const Span<ImagePaintSelectionTarget> candidates)
{
  for (const ImagePaintSelectionTarget &target : candidates) {
    Vector<SelectionTileFragment> fragments;
    if (!image_select_extract_per_tile(op, target.image, target.iuser, &fragments)) {
      continue;
    }
    state.fragments.append(std::move(fragments));
    state.targets.append(target);
  }
  if (state.fragments.is_empty()) {
    return false;
  }
  for (const int target_i : state.fragments.index_range()) {
    for (const SelectionTileFragment &frag : state.fragments[target_i]) {
      state.outline_uv.append(move3d_fragment_outlines_uv(frag));
    }
    /* Lift: clear the source region so the fragment visibly floats. */
    image_select_fragment_lift_source(
        C, state.targets[target_i].image, state.targets[target_i].iuser,
        Span<SelectionTileFragment>(state.fragments[target_i]));
  }
  if (Object *ob = state.anchor.object) {
    state.surface_map.ensure(*ob, state.anchor.uv_map_name());
  }
  return true;
}

static wmOperatorStatus move3d_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_PASS_THROUGH;
  }
  /* Re-drag path FIRST: a floating move session of this Viewport exists. A press inside its
   * fragments starts another drag gesture; a press outside settles the session (2D parity: the
   * Image Editor's Move tool keymap binds a plain click to a selection reset -- here the
   * equivalent is the commit, since the source region stays cut and the fragment has to land). */
  if (ImageSelectMove3DState *floating = move3d_state_get()) {
    if (floating->owner_v3d != CTX_wm_view3d(C)) {
      return OPERATOR_PASS_THROUGH;
    }
    if (floating->cursor_over_fragment(C, int2(event->mval[0], event->mval[1]))) {
      floating->is_dragging = true;
      floating->prev_mouse_xy = int2(event->mval[0], event->mval[1]);
      floating->drag_offset_history.append(floating->uv_drag_offset);
      if (floating->use_decal && CTX_wm_region(C) != nullptr) {
        move3d_decal_drag_begin(
            C, *floating, *CTX_wm_region(C), int2(event->mval[0], event->mval[1]));
      }
      WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
      WM_event_add_modal_handler(C, op);
      return OPERATOR_RUNNING_MODAL;
    }
    image_select_view3d_session_end(C, true);
    return OPERATOR_FINISHED;
  }
  /* Another tool's session may still float in this Viewport: settle it before this tool lifts its
   * own fragments (the session slot must be empty, see #image_select_view3d_session_set). The
   * strict poll below then sees the settled canvas like a fresh invoke. */
  image_select_view3d_session_end(C, true);
  if (!image_paint_selection_view3d_poll(C)) {
    return OPERATOR_PASS_THROUGH;
  }

  auto *state = MEM_new<ImageSelectMove3DState>(__func__);
  if (!image_select_view3d_anchor_from_mval(C, int2(event->mval[0], event->mval[1]), state->anchor)) {
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Selection: no surface under the cursor");
    return OPERATOR_CANCELLED;
  }
  const Vector<ImagePaintSelectionTarget> candidates = image_paint_selection_view3d_targets_get(
      C, *ob);
  if (!move3d_lift_fragments(C, op, *state, candidates)) {
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Selection is empty");
    return OPERATOR_CANCELLED;
  }
  /* The content mapping is resolved now that the lift fixed the fragments: Decal needs the stamps
   * and the surface patch, and falls back to the UV offset when they cannot be built. */
  if (move3d_decal_requested(C) && !move3d_decal_begin(C, *state)) {
    BKE_report(op->reports, RPT_WARNING, "Decal: no surface patch; moving in UV mode");
  }
  for (const ImagePaintSelectionTarget &target : state->targets) {
    state->borrowed_images.append(target.image);
  }
  if (ARegion *region_for_cb = CTX_wm_region(C)) {
    state->owner_region_type = region_for_cb->runtime->type;
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, move3d_draw, state, REGION_DRAW_POST_VIEW);
  }
  image_select_view3d_session_set(C, CTX_wm_view3d(C), state);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
  state->is_dragging = true;
  state->prev_mouse_xy = int2(event->mval[0], event->mval[1]);
  state->drag_offset_history.append(float2(0.0f));
  if (state->use_decal && CTX_wm_region(C) != nullptr) {
    move3d_decal_drag_begin(C, *state, *CTX_wm_region(C), state->prev_mouse_xy);
  }
  WM_event_add_modal_handler(C, op);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus move3d_modal(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  ImageSelectMove3DState *state = move3d_state_get();
  ARegion *region = CTX_wm_region(C);
  if (state == nullptr || region == nullptr || state->owner_region != region) {
    if (state != nullptr) {
      state->is_dragging = false;
      WM_cursor_modal_restore(CTX_wm_window(C));
    }
    return OPERATOR_FINISHED;
  }
  if (!state->is_dragging) {
    return OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL;
  }

  if (event->type == MOUSEMOVE) {
    const int2 mval(event->mval[0], event->mval[1]);
    if (state->use_decal) {
      move3d_decal_drag_step(C, *state, *region, mval);
      ED_region_tag_redraw(region);
      state->prev_mouse_xy = mval;
      return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
    }
    float2 plane_now;
    if (image_select_view3d_mval_to_plane_px(state->anchor, *region, mval, plane_now)) {
      float2 plane_prev = plane_now;
      if (image_select_view3d_mval_to_plane_px(
              state->anchor, *region, state->prev_mouse_xy, plane_prev))
      {
        state->uv_drag_offset += image_select_view3d_plane_px_to_uv_delta(state->anchor,
                                                                          plane_now - plane_prev);
        ED_region_tag_redraw(region);
      }
    }
    state->prev_mouse_xy = mval;
    return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
  }
  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    state->is_dragging = false;
    WM_cursor_modal_restore(CTX_wm_window(C));
    /* A mode change requested during the gesture applies now that it is over. */
    image_select_view3d_pending_mode_apply(C);
    ED_region_tag_redraw(region);
    return OPERATOR_FINISHED;
  }
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case IMAGE_SELECT_FLOATING_MODAL_CONFIRM:
        image_select_view3d_session_end(C, true);
        return OPERATOR_FINISHED;
      case IMAGE_SELECT_FLOATING_MODAL_CANCEL:
        image_select_view3d_session_end(C, false);
        return OPERATOR_FINISHED;
      case IMAGE_SELECT_FLOATING_MODAL_UNDO_STEP: {
        if (!state->drag_offset_history.is_empty()) {
          state->uv_drag_offset = state->drag_offset_history.pop_last();
          state->decal_dirty = true;
          ED_region_tag_redraw(region);
        }
        else {
          image_select_view3d_session_end(C, false);
          return OPERATOR_FINISHED;
        }
        break;
      }
      default:
        break;
    }
  }
  return OPERATOR_RUNNING_MODAL | OPERATOR_PASS_THROUGH;
}

void PAINT_OT_image_select_view3d_move(wmOperatorType *ot)
{
  ot->name = "Move Selection";
  ot->idname = "PAINT_OT_image_select_view3d_move";
  ot->description = "Move the selection across the paint canvas by dragging on the surface";
  ot->invoke = move3d_invoke;
  ot->modal = move3d_modal;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Copy / Paste
 * \{ */

static bool move3d_copy_poll(bContext *C)
{
  /* The keymap binds Ctrl+C ahead of the stack layer clipboard: the selection fragment wins the
   * key exactly while a select tool is active, and the stack layer copy keeps it otherwise. */
  if (!image_select_view3d_select_tool_active(C)) {
    return false;
  }
  if (!image_paint_selection_view3d_poll(C)) {
    return false;
  }
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  for (const ImagePaintSelectionTarget &target :
       image_paint_selection_view3d_targets_get(C, *ob))
  {
    if (BKE_image_paint_selection_mask_has_any(target.image)) {
      return true;
    }
  }
  return false;
}

/** Paste needs a copied selection: the keymap binding sits ahead of the stack layer paste, so an
 * empty clipboard must fall through to it rather than swallow the key. */
static bool move3d_paste_poll(bContext *C)
{
  if (!image_select_view3d_select_tool_active(C)) {
    return false;
  }
  const ImageSelectClipboard *clipboard = image_select_clipboard_get();
  if (clipboard == nullptr || clipboard->groups.is_empty()) {
    return false;
  }
  return image_paint_selection_view3d_tool_poll(C);
}

static wmOperatorStatus move3d_copy_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_mask_map_ensure(C);
  Vector<ImageSelectClipboardGroup> groups;
  bool has_content = false;
  for (const ImagePaintSelectionTarget &target :
       image_paint_selection_view3d_targets_get(C, *ob))
  {
    Vector<SelectionTileFragment> fragments;
    if (!image_select_extract_per_tile(op, target.image, target.iuser, &fragments)) {
      continue;
    }
    for (const SelectionTileFragment &frag : fragments) {
      bool any = false;
      image_select_fragment_content_mask(frag, any);
      has_content = has_content || any;
    }
    ImageSelectClipboardGroup group;
    group.image_uid = uint32_t(target.image->id.session_uid);
    group.fragments = std::move(fragments);
    groups.append(std::move(group));
  }
  if (!has_content) {
    /* A selection over empty (transparent) pixels has nothing to paste: keep the previous
     * clipboard instead of replacing it with a fragment that would place nothing. */
    for (ImageSelectClipboardGroup &group : groups) {
      selection_tile_fragments_free(group.fragments);
    }
    BKE_report(op->reports,
               RPT_WARNING,
               groups.is_empty() ? "Selection is empty" :
                                   "Selection contains only transparent pixels");
    return OPERATOR_CANCELLED;
  }
  ImageSelectClipboard *clipboard = image_select_clipboard_get();
  image_select_clipboard_clear();
  clipboard->groups = std::move(groups);
  clipboard->has_mask = true;
  return OPERATOR_FINISHED;
}

static wmOperatorStatus move3d_paste_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  image_paint_selection_mask_map_ensure(C);
  const ImageSelectClipboard *clipboard = image_select_clipboard_get();
  if (clipboard->groups.is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "Clipboard is empty");
    return OPERATOR_CANCELLED;
  }
  /* Both Selection Paste settings open a floating Transform session to place the fragment. */
  return image_select_paste_clipboard(C, op->reports, *ob, *clipboard) ? OPERATOR_FINISHED :
                                                                         OPERATOR_CANCELLED;
}

void PAINT_OT_image_select_view3d_copy(wmOperatorType *ot)
{
  ot->name = "Copy Selection";
  ot->idname = "PAINT_OT_image_select_view3d_copy";
  ot->description = "Copy the selection to the paint clipboard";
  ot->exec = move3d_copy_exec;
  ot->poll = move3d_copy_poll;
  ot->flag = OPTYPE_REGISTER;
}

void PAINT_OT_image_select_view3d_paste(wmOperatorType *ot)
{
  ot->name = "Paste Selection";
  ot->idname = "PAINT_OT_image_select_view3d_paste";
  ot->description =
      "Paste the clipboard selection as a floating selection centered on the surface under the "
      "view, placed with the Transform tool; the Selection Paste setting puts it in a new layer of "
      "the stack (a Material canvas) or in the current layer";
  ot->exec = move3d_paste_exec;
  ot->poll = move3d_paste_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
