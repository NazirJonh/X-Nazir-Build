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
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_move_intern.hh"
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
  /** Which target the on-surface preview shows; -1 = auto (the base-color map, else the first).
   * Cycled by #preview_channel_cycle (a Material canvas lifts one fragment set per PBR map, and
   * stacking roughness / normal maps on top of each other reads as nothing). */
  int preview_target = -1;

  explicit ImageSelectMove3DState()
      : PaintSelectView3DFloatingSession(PaintSelectView3DTool::Move)
  {
  }

  /** Resolve #preview_target: an explicit index while it stays valid, else the base-color map,
   * else the first target. */
  int preview_target_resolved() const
  {
    if (this->preview_target >= 0 && this->preview_target < this->targets.size()) {
      return this->preview_target;
    }
    for (const int target_i : this->targets.index_range()) {
      if (this->targets[target_i].is_color_channel) {
        return target_i;
      }
    }
    return 0;
  }

  void preview_channel_cycle() override
  {
    if (this->targets.is_empty()) {
      return;
    }
    const int current = std::max(this->preview_target_resolved(), 0);
    this->preview_target = (current + 1) % int(this->targets.size());
  }

  /** Free the preview textures; makes the GPU context current when the caller has none (a
   * Viewport freed while a session floats, file load, exit). */
  void preview_textures_free()
  {
    image_select_view3d_preview_textures_free(this->preview_textures);
  }

  ~ImageSelectMove3DState() override
  {
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
static void move3d_draw(const bContext * /*C*/, ARegion *region, void *arg)
{
  auto *state = static_cast<ImageSelectMove3DState *>(arg);
  if (state == nullptr || state->anchor.object == nullptr || !state->surface_map.sampler) {
    return;
  }
  /* The fragment pixels at their dragged position, on the surface; the source region is already
   * cleared on the canvas, so this is the only place the content shows while it floats. */
  if (state->preview_textures.size() != state->fragments.size()) {
    state->preview_textures.resize(state->fragments.size());
  }
  /* A Material canvas lifts one fragment set per PBR map, all covering the same UV rect: drawing
   * them all would just stack the maps, leaving the last one (roughness, normal, ...) visible.
   * Show the user-cycled target, else the base-color map (what the surface looks like). */
  const int preview_target = state->preview_target_resolved();
  /* The preview triangles lie exactly on the mesh surface: without a depth offset the shared
   * pixels z-fight and the texture shows through only in patches. */
  if (region != nullptr && region->regiondata != nullptr) {
    ED_view3d_polygon_offset(static_cast<const RegionView3D *>(region->regiondata), 1.0f);
  }
  for (const int target_i : state->fragments.index_range()) {
    if (target_i != preview_target) {
      continue;
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
            "Move3DFragment", frag.preview.fragment_display_ibuf, true, false, false);
        if (textures[frag_i] != nullptr) {
          /* Linear, not nearest: the fragment is usually minified heavily on the surface, and
           * nearest sampling drops whole texel rows there (the preview showed through only in
           * patches). No mipmaps in this texture path, so the minified rim stays a bit sharp. */
          GPU_texture_filter_mode(textures[frag_i], true);
        }
      }
      const float2 tile_size = float2(frag.geom.tile_size_px);
      const float2 uv_min = image_select_udim_tile_uv_origin(frag.geom.tile_number) +
                            float2(frag.geom.origin_px) / tile_size + state->uv_drag_offset;
      image_select_view3d_draw_uv_texture(*state->anchor.object,
                                          state->surface_map,
                                          textures[frag_i],
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

void ImageSelectMove3DState::commit(bContext *C)
{
  move3d_state_free_draw_handle(*this);
  WM_cursor_modal_restore(CTX_wm_window(C));

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
  if (this->owner_region != nullptr) {
    ED_region_tag_redraw(this->owner_region);
  }
  return true;
}

bool ImageSelectMove3DState::cursor_over_fragment(const bContext *C, const int2 &mval) const
{
  ARegion *region = CTX_wm_region(C);
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

static wmOperatorStatus move3d_copy_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectClipboard *clipboard = image_select_clipboard_get();
  image_select_clipboard_clear();
  for (const ImagePaintSelectionTarget &target :
       image_paint_selection_view3d_targets_get(C, *ob))
  {
    Vector<SelectionTileFragment> fragments;
    if (!image_select_extract_per_tile(op, target.image, target.iuser, &fragments)) {
      continue;
    }
    ImageSelectClipboardGroup group;
    group.image_uid = uint32_t(target.image->id.session_uid);
    group.fragments = std::move(fragments);
    clipboard->groups.append(std::move(group));
  }
  clipboard->has_mask = !clipboard->groups.is_empty();
  return clipboard->groups.is_empty() ? OPERATOR_CANCELLED : OPERATOR_FINISHED;
}

static wmOperatorStatus move3d_paste_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !image_paint_selection_view3d_poll(C)) {
    return OPERATOR_CANCELLED;
  }
  const ImageSelectClipboard *clipboard = image_select_clipboard_get();
  if (clipboard->groups.is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "Clipboard is empty");
    return OPERATOR_CANCELLED;
  }
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return OPERATOR_CANCELLED;
  }
  /* The paste is an exec (no event): anchor at the view center. */
  const int2 center(region->winx / 2, region->winy / 2);
  ImageSelectView3DAnchor anchor;
  if (!image_select_view3d_anchor_from_mval(C, center, anchor)) {
    BKE_report(op->reports, RPT_WARNING, "Selection: no surface under the view center");
    return OPERATOR_CANCELLED;
  }
  if (!anchor.jacobian_valid) {
    BKE_report(op->reports, RPT_WARNING, "Selection: cannot resolve the paste position");
    return OPERATOR_CANCELLED;
  }

  /* Commit any live session first: pasting while floating would mix fragments. */
  image_select_view3d_session_end(C, true);

  auto *state = MEM_new<ImageSelectMove3DState>(__func__);
  state->anchor = anchor;
  /* Resolved into a local list: #state->targets grows below in lockstep with #state->fragments,
   * so a pointer into #state->targets itself would dangle on reallocation, and pre-filling it
   * would desynchronize the fragment indices. */
  const Vector<ImagePaintSelectionTarget> candidates = image_paint_selection_view3d_targets_get(
      C, *ob);
  for (const ImageSelectClipboardGroup &group : clipboard->groups) {
    /* Match clipboard groups to the writable targets by session UID. */
    const ImagePaintSelectionTarget *target_ptr = nullptr;
    for (const ImagePaintSelectionTarget &target : candidates) {
      if (target.image != nullptr && uint32_t(target.image->id.session_uid) == group.image_uid) {
        target_ptr = &target;
        break;
      }
    }
    if (target_ptr == nullptr) {
      continue;
    }
    /* One fragment list per target: a duplicated clipboard group must not add the same image
     * twice (the parallel vectors pair by index). */
    if (std::any_of(state->targets.begin(),
                    state->targets.end(),
                    [&](const ImagePaintSelectionTarget &target) {
                      return target.image == target_ptr->image;
                    }))
    {
      continue;
    }
    Vector<SelectionTileFragment> fragments;
    for (const SelectionTileFragment &src : group.fragments) {
      SelectionTileFragment copy = {};
      copy.pixels.fragment_ibuf = IMB_dupImBuf(src.pixels.fragment_ibuf);
      if (src.pixels.fragment_mask_ibuf != nullptr) {
        copy.pixels.fragment_mask_ibuf = IMB_dupImBuf(src.pixels.fragment_mask_ibuf);
        copy.preview.fragment_blend_mask_ibuf = IMB_dupImBuf(
            src.preview.fragment_blend_mask_ibuf);
        copy.preview.fragment_display_ibuf = IMB_dupImBuf(src.preview.fragment_display_ibuf);
        if (src.preview.fragment_feather_display_ibuf != nullptr) {
          copy.preview.fragment_feather_display_ibuf = IMB_dupImBuf(
              src.preview.fragment_feather_display_ibuf);
        }
      }
      copy.geom = src.geom;
      copy.edge_policy = src.edge_policy;
      copy.is_pasted = true;
      fragments.append(std::move(copy));
    }
    if (fragments.is_empty()) {
      continue;
    }
    state->fragments.append(std::move(fragments));
    /* A copy of the value: \a target_ptr points into the local #candidates, but the appended
     * element must own its data after the loop. */
    state->targets.append(*target_ptr);
  }
  if (state->fragments.is_empty()) {
    MEM_delete(state);
    BKE_report(op->reports, RPT_WARNING, "Clipboard images are not on the current canvas");
    return OPERATOR_CANCELLED;
  }

  /* Center the pasted fragments on the anchor: offset = anchor_uv - first fragment center. */
  {
    const SelectionTileFragment &first = state->fragments[0].first();
    const float2 tile_uv = image_select_udim_tile_uv_origin(first.geom.tile_number);
    const float2 center_px = float2(first.geom.origin_px) +
                             float2(first.geom.size_px) * 0.5f;
    const float2 frag_center_uv = tile_uv + center_px / float2(first.geom.tile_size_px);
    state->uv_drag_offset = anchor.anchor_uv - frag_center_uv;
  }

  for (const Vector<SelectionTileFragment> &per_target : state->fragments) {
    for (const SelectionTileFragment &frag : per_target) {
      state->outline_uv.append(move3d_fragment_outlines_uv(frag));
    }
  }
  if (anchor.object != nullptr) {
    state->surface_map.ensure(*anchor.object, anchor.uv_map_name());
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
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  ED_region_tag_redraw(CTX_wm_region(C));
  return OPERATOR_FINISHED;
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
  ot->description = "Paste the clipboard selection centered on the surface under the view";
  ot->exec = move3d_paste_exec;
  ot->poll = image_paint_selection_view3d_tool_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

}  // namespace blender::ed::sculpt_paint
