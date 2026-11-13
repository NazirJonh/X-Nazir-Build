/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared state and internal helpers of the 3D Viewport "Transform Selection" tool, split across
 * `paint_image_select_transform_view3d.cc` (state / session / operators),
 * `paint_image_select_transform_view3d_draw.cc` (POST_VIEW / POST_PIXEL overlay) and
 * `paint_image_select_transform_view3d_drag.cc` (drag interaction).
 */

#pragma once

#include <cstdint>
#include <string>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "BKE_bvhutils.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_paint_layers.hh"

#include "../projection/projection_target.hh"
#include "paint_image_select_floating.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_move_intern.hh"
#include "paint_image_select_surface_decal.hh"
#include "paint_image_select_surface_unfold.hh"
#include "paint_image_select_view3d.hh"

struct ARegion;
struct ImBuf;
struct Image;
struct Material;
struct bContext;
struct wmOperator;

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

namespace image_select_v3d {

enum class Transform3DHandle {
  None,
  Move,
  Rotate,
  Pivot,
  Corner0,
  Corner1,
  Corner2,
  Corner3,
  /** Edge midpoints of the cage (bottom, right, top, left): scale along one axis only. */
  Edge0,
  Edge1,
  Edge2,
  Edge3,
};

/**
 * The content mapping a Transform Selection session authors, read from the tool settings at
 * invoke. #UV keeps the historical affine in UV space; #Decal carries the content over a surface
 * patch unfolded from the mesh triangles around a frame that follows the surface, so seams,
 * islands, UV gaps and cube edges do not tear the content. The cage, the handles and the gestures
 * are shared: in #Decal the state keeps a UV-equivalent affine synced from the decal frame (see
 * #transform3d_decal_sync_affine), so the overlay code reads the same fields in both modes.
 */
enum class Transform3DContentMode {
  UV = 0,
  Decal = 1,
};

/** One finished gesture's transform snapshot, for step-back. */
struct Transform3DSnapshot {
  float2x2 linear;
  float2 pivot;
  float2 translation;
  /** The decal state of the gesture, read when the session runs #Transform3DContentMode::Decal. */
  float2 decal_center_uv = float2(0.0f);
  float decal_roll = 0.0f;
  float2 decal_scale = float2(1.0f);
};

/* #transform_uv and #plane_rotation live in `paint_image_select_surface_decal.hh`, which the shared
 * decal core needs as well. */

}  // namespace image_select_v3d

/**
 * Decal-mode session data (stamps, surface patch, frame): the shared surface-decal core's session,
 * see #SurfaceDecalSession. In this tool the frame is authored by the 3D gestures; the Image
 * Editor's sessions author it from their UV gestures.
 */
using ImageSelectTransform3DDecal = SurfaceDecalSession;

/** Free the stamps and drop the patch. Idempotent; safe on every exit path. */
void transform3d_decal_release(ImageSelectTransform3DDecal &decal);

struct ImageSelectTransform3DState : PaintSelectView3DFloatingSession {
  ImageSelectView3DAnchor anchor;
  Vector<ImagePaintSelectionTarget> targets;
  Vector<Vector<SelectionTileFragment>> fragments;
  /**
   * The transform is authoritative in UV space: `dest = pivot + linear * (src - pivot) +
   * translation`. Authoring it here, instead of in the anchor plane, is what lets the tool follow
   * the surface: the plane is re-sampled under the fragment as it moves, while the content mapping
   * stays one affine until the projection backend takes it over.
   */
  float2x2 uv_linear = float2x2::identity();
  float2 uv_pivot = float2(0.0f);
  float2 uv_translation = float2(0.0f);
  /**
   * Orientation of the fragment frame on the anchor plane: the minimal oriented rectangle of the
   * selection content (a box select gives its own edges), so the cage and the scale axes follow
   * the selection instead of the screen or the canvas. #plane_min / #plane_max are in this frame.
   */
  float frame_angle = 0.0f;
  /** The cage corners in *source* UV, in the handle order (bottom-left, bottom-right, top-right,
   * top-left of the fragment frame). */
  float2 source_corner_uv[4] = {float2(0.0f), float2(0.0f), float2(0.0f), float2(0.0f)};
  bool source_corners_valid = false;
  /**
   * The surface-local frame under the transformed pivot, refreshed as the fragment moves. The
   * cage, the rotate/scale drag plane and the off-surface fallback all use it, so the gizmo turns
   * with the face it is on instead of staying in the plane it was created in.
   */
  ImageSelectView3DAnchor display_anchor;
  bool display_frame_valid = false;
  /** UV the display frame was sampled at, to skip redundant re-samples. */
  float2 display_uv = float2(0.0f);
  /** Frame captured at a press and held for the gesture (rotate / scale keep one plane). */
  ImageSelectView3DAnchor gesture_anchor;
  /** Cage edges sampled on the surface (region px polylines), so the cage bends over an edge. */
  Vector<float2> screen_cage[4];
  /** The cursor's last surface point, for the object-space move delta that avoids UV-seam jumps. */
  float2 prev_surface_uv = float2(0.0f);
  float3 prev_surface_co = float3(0.0f);
  bool prev_surface_valid = false;
  Vector<image_select_v3d::Transform3DSnapshot> drag_history;
  ImageSelectView3DSurfaceMap surface_map;
  /** Fragment outlines in source UV space (segment pairs). */
  Vector<Vector<float2>> outline_uv;
  /** GPU copies of the fragments' display buffers for the on-surface preview, created lazily by
   * the texture draw callback (a GPU context is current there), parallel to #fragments. */
  Vector<Vector<gpu::Texture *>> preview_textures;
  /** Which target the on-surface preview shows; -1 = the PBR composite (a Material canvas's maps
   * shaded together), or the auto target when none is possible. Cycled by #preview_channel_cycle
   * between the composite and the channels it cannot shade (stacking roughness / normal maps flat
   * on top of each other reads as nothing). */
  int preview_target = -1;
  /** Neutral 1x1 stand-ins of the PBR composite, one per #PBRPreviewChannel. */
  gpu::Texture *pbr_default_textures[PBR_PREVIEW_CHANNEL_NUM] = {nullptr};
  /** Pasted content has no selection of its own on the canvas: the yellow selection outline is not
   * drawn for it, only the cage handles. */
  bool hide_outline = false;
  /** POST_VIEW draw handle of the texture preview. Two callbacks because the handles / outline
   * draw in region px (#draw_handle, POST_PIXEL) while the fragment pixels draw through the
   * view-projection matrix (this one, POST_VIEW). */
  void *draw_handle_texture = nullptr;
  /**
   * Bounds of the fragment's *source* cage in the anchor plane px. The destination cage is these
   * corners carried through the UV affine and sampled back onto the surface (#source_corner_uv).
   */
  float2 plane_min = float2(0.0f);
  float2 plane_max = float2(0.0f);
  bool plane_bounds_valid = false;
  /** Screen-space handle positions, refreshed by the draw callback. */
  float2 screen_corners[4] = {float2(0.0f), float2(0.0f), float2(0.0f), float2(0.0f)};
  float2 screen_edges[4] = {float2(0.0f), float2(0.0f), float2(0.0f), float2(0.0f)};
  float2 screen_rotate = float2(0.0f);
  float2 screen_pivot = float2(0.0f);
  bool screen_valid = false;
  /** Handle under the cursor while idle, for the hover highlight. */
  image_select_v3d::Transform3DHandle hover_handle = image_select_v3d::Transform3DHandle::None;
  /** Live drag. */
  image_select_v3d::Transform3DHandle active_handle = image_select_v3d::Transform3DHandle::None;
  image_select_v3d::Transform3DSnapshot drag_start;
  int2 press_xy = int2(0);
  /** Press position on the gesture frame's plane (plane px), captured at drag start. */
  float2 press_plane = float2(0.0f);
  /** The content mapping of this session, read from the tool settings at invoke (Decal falls back
   * to UV when the object's material cannot take rows). */
  image_select_v3d::Transform3DContentMode mode = image_select_v3d::Transform3DContentMode::UV;
  /** Decal-mode data (stamps, surface patch, the frame); read only when #mode is Decal. */
  ImageSelectTransform3DDecal decal;
  /** Set when the decal state changed outside a frame sync (a step-back, a restore): the next
   * draw callback re-syncs the frame and the patch (it is the one caller that always has a
   * context). Cleared by #transform3d_decal_frame_sync. */
  bool decal_frame_dirty = false;

  explicit ImageSelectTransform3DState()
      : PaintSelectView3DFloatingSession(PaintSelectView3DTool::Transform)
  {
  }

  /** Resolve #preview_target: an explicit index while it stays valid, else the base-color map,
   * else the first target. */
  int preview_target_resolved() const
  {
    return image_select_preview_target_resolve(this->targets, this->preview_target);
  }

  void preview_channel_cycle() override
  {
    this->preview_target = image_select_preview_target_cycle(this->targets, this->preview_target);
  }

  ~ImageSelectTransform3DState() override
  {
    transform3d_decal_release(this->decal);
    image_select_view3d_preview_textures_free(this->preview_textures);
    for (gpu::Texture *&tex : this->pbr_default_textures) {
      image_select_view3d_preview_texture_free(tex);
    }
    for (Vector<SelectionTileFragment> &per_target : this->fragments) {
      selection_tile_fragments_free(per_target);
    }
    this->fragments.clear();
  }

  void commit(bContext *C) override;
  void cancel(bContext *C) override;
  bool undo_step() override;
  bool cursor_over_fragment(const bContext *C, const int2 &mval) const override;

  /* Current transform in UV space. */
  float2x2 linear_uv() const;
  float2 pivot_uv() const;
  float2 translation_uv() const;

  ImageSelectView3DSurfaceMap &surface_map_ref()
  {
    return this->surface_map;
  }
};

using image_select_v3d::Transform3DHandle;
using image_select_v3d::Transform3DSnapshot;

/**
 * The content mapping of the floating transform: where a source canvas texel goes. Today this is
 * the affine the fragment writer applies. A projected backend takes the same source/destination
 * frames (#projection) and replaces only the mapping; the gizmo, the cage and the gestures do not
 * depend on the backend.
 */
struct ImageSelectTransform3DMapping {
  float2x2 linear;
  float2 pivot;
  float2 translation;
  bool frames_valid = false;
  projection::ProjectionFrame projection;
};

/* Helpers shared by the translation units of the tool (defined in the files named above). */

ImageSelectTransform3DMapping transform3d_mapping_get(
    const ImageSelectTransform3DState &state);

bool transform3d_refresh_display_anchor(const bContext *C,
                                        ImageSelectTransform3DState &state,
                                        const bool force);

void transform3d_refresh_screen(ImageSelectTransform3DState &state, const ARegion &region);

bool transform3d_uv_to_screen(ImageSelectTransform3DState &state,
                              const ARegion &region,
                              const float2 &uv,
                              float2 &r_screen);

Transform3DSnapshot transform3d_snapshot(const ImageSelectTransform3DState &state);

void transform3d_restore(ImageSelectTransform3DState &state,
                         const Transform3DSnapshot &snap);

/**
 * Resolve the cursor onto the surface under \a anchor's frame: the object-space point and the UV
 * it samples (the raycast the drag gestures share, defined in the drag unit; the decal gestures
 * read the same point for the frame center).
 */
bool transform3d_surface_point_at(bContext *C,
                                  ImageSelectTransform3DState &state,
                                  const ImageSelectView3DAnchor &anchor,
                                  const ARegion &region,
                                  const int2 &mval,
                                  float2 &r_uv,
                                  float3 &r_co);

void transform3d_apply_handle(bContext *C,
                              ImageSelectTransform3DState &state,
                              const ARegion &region,
                              const int2 &mval,
                              const bool ctrl);

bool transform3d_begin_drag(bContext *C,
                            ImageSelectTransform3DState &state,
                            const int2 &mval);

void transform3d_draw(const bContext *C, ARegion *region, void *arg);

void transform3d_draw_texture(const bContext * /*C*/, ARegion *region, void *arg);

/* -------------------------------------------------------------------- */
/** \name Decal mode
 *
 * The surface-following content mapping (see #Transform3DContentMode::Decal): stamps built from
 * the lifted fragments in the cage's own frame, the unfolded surface patch they ride, the surface
 * frame that places them, the draw-overlay preview and the commit. Defined in
 * `paint_image_select_transform_view3d_decal.cc`.
 * \{ */

/**
 * Read the tool settings' transform mode and resolve it for \a state's object: #Decal stays for a
 * mesh object, otherwise the session falls back to #UV. Called from the invoke before the decal
 * session opens.
 */
void transform3d_mode_resolve(bContext *C,
                              ImageSelectTransform3DState &state,
                              wmOperator *op);

/**
 * Open the decal half of a session: build one stamp per target from the lifted fragments
 * (re-sampled from UV into the cage's own frame, soft selection coverage as alpha, Normal vectors
 * rewritten into the frame basis) and the surface patch around the frame center.
 * \return false when nothing could be created; the caller then falls back to the UV mode.
 */
bool transform3d_decal_begin(bContext *C,
                             ImageSelectTransform3DState &state,
                             wmOperator *op);

/**
 * Switch a live session between #Transform3DContentMode::UV and ::Decal without losing the gesture
 * done so far. Decal -> UV releases the decal (the affine is already synced from it); UV -> Decal
 * opens the decal at the frame the current affine describes (the affine decomposed into center,
 * roll and per-axis scale; a shear is dropped) and restarts the drag history. While a drag runs the
 * change is kept in #PaintSelectView3DFloatingSession::pending_decal and applied when the drag ends;
 * the current mode stays when the decal cannot be opened.
 * \return true when the mode changed.
 */
bool transform3d_mode_switch(bContext *C, ImageSelectTransform3DState &state, bool want_decal);

/**
 * Re-sample the surface frame at the decal center, sync the UV-equivalent affine the overlay
 * reads, and refresh the surface patch when the center moved or the stamp outgrew it. Safe to call
 * on every drag step. Keeps the last valid frame and patch when the center has no surface under it.
 */
void transform3d_decal_frame_sync(const bContext *C, ImageSelectTransform3DState &state);

/** The decal frame as the projection system's inline frame (object-space center, extents,
 * normal + roll). False while the display frame is not (yet) valid. */
bool transform3d_decal_frame_get(const ImageSelectTransform3DState &state,
                                 projection::ProjectionFrame &r_frame);

/** The per-handle application in #Transform3DContentMode::Decal (move, rotate, scale against the
 * decal frame). The gesture plane is the display anchor's, which is anchored at the frame center. */
void transform3d_decal_apply_handle(bContext *C,
                                    ImageSelectTransform3DState &state,
                                    const ARegion &region,
                                    const int2 &mval,
                                    const bool ctrl);

/**
 * The POST_VIEW preview of a #Transform3DContentMode::Decal session: the patch triangles at their
 * surface positions, textured with the stamp of the preview target (or the PBR composite of all of
 * them). Draws nothing into the material; needs the GPU context the draw callback runs in.
 */
void transform3d_decal_draw_preview(const ARegion *region, ImageSelectTransform3DState &state);

/** The commit of a #Transform3DContentMode::Decal session: the stamps are baked into the session's
 * canvas targets through the surface patch (one image undo step, the source region is cut). */
void transform3d_decal_commit(bContext *C, ImageSelectTransform3DState &state);

/** \} */

}  // namespace blender::ed::sculpt_paint
