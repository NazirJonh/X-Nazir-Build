/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Selection masks in the 3D Viewport (Sculpt Mode texture painting).
 *
 * The selection mask itself is the Image-Editor-owned runtime data on #bke::ImageRuntime (see
 * #BKE_image_paint_selection.hh): one single-channel float #ImBuf per UDIM tile, in UV space.
 * This module adds the 3D half of the tool family: operators that run in the 3D Viewport and
 * rasterize into those same UV-space masks through the mesh, so a selection made by stroking on
 * the surface constrains painting in both editors.
 *
 * Projection model (mirrors the 3D paint-shape tools, see `shapes/paint_shape_space.hh`): every
 * tool anchors a tangent plane to the surface under the first click (#ImageSelectView3DAnchor).
 * Screen input maps onto that plane (region pixels, "plane px"), mesh texels map back onto it
 * through #shape::SurfaceAnchoredSpace semantics (front-facing + depth rejection), and drag
 * offsets translate from plane px into UV space through the affine Jacobian of the anchor
 * triangle. A #geometry::ReverseUVSampler maps UV-space outlines back onto the surface for the
 * 3D overlay.
 *
 * Flow (high level, mirroring the 2D family in paint_image_select_intern.hh):
 * \code{.unparsed}
 *   gesture (select_view3d.cc) --> per-texel projection pass writes the UV-space mask
 *        |
 *        v
 *   paint (sculpt_paint_image.cc + paint_image_2d.cc) --> mask multiplies brush strength
 *        |
 *        v
 *   move/transform/warp (view3d floating tools) --> lift fragment --> drag on the anchor plane
 *        |                                                   |
 *        |                                                   v
 *        |                                        3D overlay (outline + handles)
 *        v
 *   commit --> write pixels + merge mask back to canvas (one image undo step)
 * \endcode
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "BLI_array.hh"
#include "BLI_function_ref.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "ED_select_utils.hh"

#include "../shapes/paint_shape_space.hh"
#include "paint_image_select_intern.hh"

/* Only a forward declaration of #blender::geometry::ReverseUVSampler is needed here (the surface
 * map owns it through a #std::unique_ptr); translation units that sample it include
 * GEO_reverse_uv_sampler.hh themselves, like the module's .cc files do below. */

struct ARegion;
struct Depsgraph;
struct Main;
struct Image;
struct ImageUser;
struct Object;
struct View3D;
struct bContext;
struct wmOperator;
struct wmOperatorType;

namespace blender::geometry {
class ReverseUVSampler;
}

namespace blender::gpu {
class Texture;
}

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Anchor and plane mapping
 * \{ */

/**
 * The tangent plane a 3D selection tool works on, frozen at the operator's first click.
 *
 * The plane lives in object space (like #shape::SurfaceAnchor): gestures are authored in "plane
 * px" -- the output space of the surface projection -- so an on-screen drag of N pixels moves the
 * selection by N plane pixels regardless of zoom, exactly like a paint shape does.
 */
struct ImageSelectView3DAnchor {
  /** The surface point, normal, tangent, `px_per_unit` and restoration cache. */
  shape::SurfaceAnchor anchor = {};
  /** Orthonormalized frame of #anchor (the single definition of the plane basis). */
  shape::SurfaceAnchorFrame frame = {};
  /**
   * The (original) object the anchor was built on. Valid for one operator invocation: the active
   * object cannot change while a modal selection gesture runs. Sessions keep their own copy.
   */
  Object *object = nullptr;
  /** Rejection depth along the plane normal, object units (0 = unlimited). */
  float max_depth = 0.0f;

  /** Affine plane-px -> UV mapping of the anchor triangle: `uv = anchor_uv + jacobian *
   * plane_px`. Built from the anchor's triangle at creation; invalid when the anchor has no UV
   * cache (then only pixel-space rasterization works, not drag mapping). */
  float2x2 plane_uv_jacobian = float2x2::identity();
  bool jacobian_valid = false;
  float2 anchor_uv = float2(0.0f);

  /**
   * View mode of the Select Box / Circle gestures: "plane px" are plain region pixels and texels
   * are projected through the view instead of onto the tangent plane. Only the gestures set it;
   * the floating tools keep the plane semantics, their drags map through #anchor.
   */
  struct ScreenSpace {
    /** Object space -> region pixels (clip space), see #ED_view3d_ob_project_mat_get. */
    float4x4 projmat = float4x4::identity();
    float2 region_size = float2(0.0f);
    /** View position and direction in object space, for the front-facing test. */
    float3 view_origin = float3(0.0f);
    float3 view_forward = float3(0.0f);
    bool is_persp = true;
    /** Per original face: seen from the view (select ID buffer). Empty = no occlusion test. */
    Array<bool> face_visible;
  };
  std::optional<ScreenSpace> screen;

  /** Region pixels of an object-space point; false when it is behind the view. */
  bool project_screen(const float3 &co_object, float2 &r_px) const;

  /** UV map the anchor was sampled through; empty = the object's active UV map. */
  StringRefNull uv_map_name() const
  {
    return anchor.surface_uv_map;
  }

  /** Map an object-space point onto the plane, returning plane px with the anchor at the origin
   * and the shared acceptance rules applied (false = reject the point). */
  bool project_object_point(const float3 &co_object, const float3 &no_object, float2 &r_px) const;

  /** Object-space point of a plane-px coordinate (the exact inverse of the projection above). */
  float3 plane_px_to_object(const float2 &px) const;
};

/**
 * Raycast the surface under \a mval and build the anchor there (mirrors the 3D paint-shape
 * invoke: view-horizontal tangent, zoom-scaled `px_per_unit`, UV/triangle restoration cache and
 * the plane-px -> UV Jacobian of the hit triangle). False when there is no surface under the
 * cursor; the caller refuses the operator.
 *
 * \param use_screen_space: fill #ImageSelectView3DAnchor::screen (View mode).
 */
bool image_select_view3d_anchor_from_mval(bContext *C,
                                          const int2 &mval,
                                          ImageSelectView3DAnchor &r_anchor,
                                          bool use_screen_space = false);

/**
 * Map a region position onto the anchor plane, returning plane px relative to the anchor.
 * False when the view ray is (near-)parallel to the plane; the caller keeps its last valid point.
 */
bool image_select_view3d_mval_to_plane_px(const ImageSelectView3DAnchor &anchor,
                                          const ARegion &region,
                                          const int2 &mval,
                                          float2 &r_px);

/** UV delta that corresponds to a plane-px delta, through the anchor's Jacobian. */
float2 image_select_view3d_plane_px_to_uv_delta(const ImageSelectView3DAnchor &anchor,
                                                const float2 &plane_px_delta);

/**
 * Project a plane-px point onto the surface (nearest surface point from the object-space position
 * on the plane) and return its UV. Used where a *point* must follow the surface -- warp grid
 * handles, pivot placement -- as opposed to a *delta*, which uses the cheaper Jacobian above.
 * False when the surface has no UV-resolvable point there.
 */
bool image_select_view3d_plane_px_to_surface_uv(bContext *C,
                                                const ImageSelectView3DAnchor &anchor,
                                                const float2 &plane_px,
                                                float2 &r_uv);

/**
 * Operator poll for the whole 3D selection family: Sculpt Mode, a window region of a Viewport,
 * a resolvable paint canvas, no live floating session and a canvas not borrowed by another
 * editor. Also the poll of the all / none / invert operators.
 */
bool image_paint_selection_view3d_poll(bContext *C);
/**
 * #image_paint_selection_view3d_poll, or a live floating session of this Viewport.
 *
 * The poll of the floating tools (move / transform / warp), the gesture operators and paste:
 * WM checks the poll *before* the invoke, so with the strict poll above their re-drag / takeover
 * paths in the invoke callbacks were unreachable -- the keymap press died at the poll exactly
 * when a fragment was already floating (the Image Editor's move poll lets its own session
 * through for the same reason). Whether the event re-drags the session, commits it or starts a
 * new gesture is decided in the invoke callbacks.
 */
bool image_paint_selection_view3d_tool_poll(bContext *C);
/** True while a floating selection session of this Viewport is live. */
bool image_paint_selection_view3d_floating_poll(bContext *C);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Targets, undo and shared edits
 * \{ */

/**
 * The writable image canvases a 3D selection edit applies to, resolved for the active object the
 * same way a sculpt paint stroke resolves its targets (Image canvas -> the canvas image,
 * Material canvas -> the enabled Principled maps).
 */
Vector<ImagePaintSelectionTarget> image_paint_selection_view3d_targets_get(bContext *C,
                                                                          Object &ob);

/**
 * First resolved target (see above): the single image whose mask the overlay displays. Shared by
 * the tools and the overlay so they never disagree on the canvas. False when nothing resolves.
 */
bool image_paint_selection_view3d_primary_target_get(bContext *C,
                                                     Object &ob,
                                                     ImagePaintSelectionTarget &r_target);

/** Begin one image undo step (#PaintMode::Sculpt) and snapshot every target's mask tiles. */
void image_paint_selection_view3d_undo_begin(const char *name,
                                             Span<ImagePaintSelectionTarget> targets);

/**
 * Write floating fragments back onto \a ima under an affine UV transform:
 * `dest_uv = pivot_uv + linear_uv * (src_uv - pivot_uv) + translation_uv`, with bilinear pixel
 * resampling, feather/binary mask weighting and destination selection-mask update. With
 * \a is_normal_map (a tangent-space normal map) the stored vectors turn with the rotation of
 * \a linear_uv, see #image_select_normal_rotation. The shared
 * write-back of the 3D move (identity linear) and transform tools. Call from inside
 * #image_select_fragment_commit_with_undo's write_final (it registers extra tiles into the open
 * undo step itself).
 */
void image_select_view3d_fragments_write_final(bContext *C,
                                               Image *ima,
                                               const ImageUser &base_iuser,
                                               const Span<SelectionTileFragment> fragments,
                                               const float2x2 &linear_uv,
                                               const float2 &pivot_uv,
                                               const float2 &translation_uv,
                                               bool is_normal_map = false);

/**
 * Commit the floating fragments of one or more canvas targets with \a write_final_per_target
 * inside ONE image undo step, so a single Ctrl+Z restores every canvas together.
 *
 * \a targets and \a fragments_per_target are parallel; the caller pre-filters them to the
 * committable entries (live images, non-empty fragment lists). The single-target case delegates
 * to #image_select_fragment_commit_with_undo; the multi-target case (a Material canvas lifts one
 * fragment set per PBR map) restores every source, snapshots all of them into one open step, and
 * only then writes -- per-target undo steps would leave the material half-edited between Ctrl+Z
 * presses (the 2D warp's #image_select_warp_commit_material_targets does the same).
 *
 * \a write_final_per_target performs the full write for one target, including the re-lift of the
 * source region (the commit restores the sources before snapshotting, exactly like
 * #image_select_fragment_commit_with_undo does).
 */
void image_select_view3d_commit_targets_with_undo(
    bContext *C,
    const char *undo_label,
    const Span<ImagePaintSelectionTarget> targets,
    const Span<Span<SelectionTileFragment>> fragments_per_target,
    const FunctionRef<void(Image &ima,
                           const ImageUser &iuser,
                           Span<SelectionTileFragment> fragments)> write_final_per_target);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask rasterization through the mesh
 * \{ */

/**
 * Rasterize a 2D predicate over the object's projected mesh texels into the targets' selection
 * masks.
 *
 * For every texel of every \a target image whose 3D position (per mesh symmetry pass) projects
 * onto the anchor plane inside \a domain_px and satisfies \a inside_fn, writes \a fill_value into
 * that image's selection-mask tile. Pixels on back-facing or too-deep geometry are rejected by
 * the anchor's acceptance rules, so a stroke on the surface selects exactly what it touches.
 *
 * \param r_touched_faces: optional. Collects the face indices owning at least one written texel
 * (of any target), for the FACE / ISLAND expansion modes.
 * \return true when at least one mask texel was written.
 */
bool image_select_view3d_rasterize_masks(bContext *C,
                                         Object &ob,
                                         const Depsgraph &depsgraph,
                                         Span<ImagePaintSelectionTarget> targets,
                                         const ImageSelectView3DAnchor &anchor,
                                         const rctf &domain_px,
                                         FunctionRef<bool(const float2 &plane_px)> inside_fn,
                                         float fill_value,
                                         Vector<int> *r_touched_faces);

/**
 * Apply one selection edit: end floating sessions, snapshot the masks into an undo step, run the
 * rasterizer, apply the FACE / ISLAND expansion for \a expand_mode, set \a edge_policy and
 * notify. The shared body of the 3D box / lasso / circle / polyline operators.
 *
 * \param undo_name: undo step label ("Box Select" etc.).
 * \param sel_op: NEW / ADD / SUB from the operator's mode property. NEW frees the masks first,
 * SUB writes 0, ADD writes 1.
 */
bool image_select_view3d_gesture_apply(bContext *C,
                                       Object &ob,
                                       const ImageSelectView3DAnchor &anchor,
                                       const rctf &domain_px,
                                       FunctionRef<bool(const float2 &plane_px)> inside_fn,
                                       PaintSelectionEdgePolicy edge_policy,
                                       const char *undo_name,
                                       eSelectOp sel_op);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Floating sessions (move / transform / warp)
 * \{ */

enum class PaintSelectView3DTool {
  Move,
  Transform,
  Warp,
};

/**
 * Base state of a live floating selection tool in the 3D Viewport.
 *
 * One session at a time is kept in a module-level slot (a Viewport owns no runtime slot of its
 * own); exclusivity on the canvas is enforced through the same `paint_selection_borrowed_by`
 * field the Image Editor uses, with the #View3D as the borrower, so a floating 3D session
 * blocks Image-Editor selection edits and painting, and vice versa.
 */
struct PaintSelectView3DFloatingSession {
  const PaintSelectView3DTool tool;
  /** The Viewport that owns the session (borrow id). Never null while the session is live. */
  View3D *owner_v3d = nullptr;
  /** Concrete region the session was started in; validated on every modal / draw use. */
  ARegion *owner_region = nullptr;
  /** Region *type* the preview draw callback is registered on. */
  ARegionType *owner_region_type = nullptr;
  ImageUser iuser = {};
  /** Handle returned by #ED_region_draw_cb_activate, removed on every exit path. */
  void *draw_handle = nullptr;
  /** True only while a mouse drag is in progress. */
  bool is_dragging = false;
  /** Canvas images this session borrowed (`paint_selection_borrowed_by`); cleared on end. */
  Vector<Image *> borrowed_images;
  /**
   * The Main the session's images live in, captured at session start. A file load / memfile
   * undo can free and re-create the canvas images while a session floats (the session cannot
   * intercept undo); every write path must verify liveness through #BKE_libblock_find_session_uid
   * before touching a stored Image pointer (the same guard the paint-shape backend uses).
   */
  Main *bmain = nullptr;

  explicit PaintSelectView3DFloatingSession(PaintSelectView3DTool t) : tool(t) {}
  virtual ~PaintSelectView3DFloatingSession() = default;

  /** Write the final result and end the session (Enter / takeover). */
  virtual void commit(bContext *C) = 0;
  /** Restore the pre-session state and end the session (Esc). */
  virtual void cancel(bContext *C) = 0;
  /**
   * Revert the last drag step (Ctrl+Z while the session floats). True when a step was undone;
   * false when there is nothing left, in which case the unified undo-step operator ends the
   * session with a cancel.
   */
  virtual bool undo_step()
  {
    return false;
  }
  /** True when the cursor is over one of the session's fragments (gesture delegation). */
  virtual bool cursor_over_fragment(const bContext *C, const int2 &mval) const = 0;

  /**
   * Cycle which of the session's canvas images the on-surface fragment preview shows (a Material
   * canvas lifts one fragment set per PBR map; only one can be drawn without stacking). Tools
   * without a texture preview keep the empty default.
   */
  virtual void preview_channel_cycle() {}
};

/** The live floating session of the active Viewport, or null. */
PaintSelectView3DFloatingSession *image_select_view3d_session_active();

/** Typed accessor to the live floating session: null unless it runs \a tool. */
template<typename T, PaintSelectView3DTool tool>
T *image_select_view3d_session_get()
{
  PaintSelectView3DFloatingSession *session = image_select_view3d_session_active();
  if (session == nullptr || session->tool != tool) {
    return nullptr;
  }
  return static_cast<T *>(session);
}

/** Store \a session (the slot must be empty) and borrow the canvases for \a owner_v3d. */
void image_select_view3d_session_set(bContext *C,
                                     View3D *owner_v3d,
                                     PaintSelectView3DFloatingSession *session);

/** Remove the session's preview draw callback (no-op when nothing is registered). */
void image_select_view3d_floating_draw_handle_clear(PaintSelectView3DFloatingSession &session);

/**
 * End the live session if there is one: \a commit selects between #commit and #cancel. Returns
 * true when a session was ended. Takeover callers commit; cancel callers restore.
 */
bool image_select_view3d_session_end(bContext *C, bool commit);

/** Free the live session if it belongs to \a v3d (Viewport close; no context needed). */
void image_select_view3d_space_free(View3D *v3d);

/**
 * True when \a image is still the same ID in the session's Main (not freed / re-created by a
 * file load or memfile undo since the session started). False also for a null \a image.
 */
bool image_select_view3d_image_is_alive(const PaintSelectView3DFloatingSession &session,
                                        const Image *image);

/** True when the active object's canvases are borrowed by another editor (poll guard). */
bool image_select_view3d_canvas_borrowed_elsewhere(bContext *C);
/** Variant taking already resolved targets, so polls resolve the (costly) target list once. */
bool image_select_view3d_canvas_borrowed_elsewhere(bContext *C,
                                                   Span<ImagePaintSelectionTarget> targets);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Surface map and outline (3D overlay)
 * \{ */

/**
 * UV -> surface mapping for the 3D overlay: a #geometry::ReverseUVSampler over owned copies of
 * the mesh's corner UVs and corner triangles, so overlay samples never dangle on a re-evaluated
 * mesh. Built lazily per (object, UV map, topology) and shared by the static selection outline
 * and the floating tools' fragment outlines.
 */
struct ImageSelectView3DSurfaceMap {
  Array<float2> uv_map;
  Array<int3> corner_tris;
  std::unique_ptr<blender::geometry::ReverseUVSampler> sampler;
  /** Identity guards: rebuild when any of these changes. */
  uint32_t object_session_uid = 0;
  int64_t corner_tri_num = 0;
  std::string uv_map_name = "";

  /** (Re)build for \a ob's mesh if the identity guards say so. Returns false when the mesh has
   * no usable UV map. The sampler works on the original mesh's UV layout (the space the masks
   * and every other tool use); positions are passed per sample so a deformation is followed
   * without a rebuild. */
  bool ensure(const Object &ob, StringRef uv_name);

  /** Sample \a uv to an object-space position + normal: `corner_verts` / `vert_positions` come
   * from the same mesh the sampler was built on (positions may be the evaluated ones). */
  bool sample(const Span<int> corner_verts,
              const Span<float3> vert_positions,
              const float2 &uv,
              float3 &r_co,
              float3 &r_no) const;
};

/**
 * Boundary segments of one tile's selection mask in global UV space: four border segments per
 * selected pixel adjacent to an unselected one, pixel-cell corners in tile UV. Used by both the
 * static outline and the fragment outlines.
 *
 * \param step: decimation stride in pixels (1 = every boundary pixel).
 */
Vector<float2> image_select_view3d_mask_boundary_uv(const Image &image,
                                                    int tile_number,
                                                    const ImageUser &iuser,
                                                    int step);

/**
 * Shared core behind the mask boundary walkers: four border segments per selected pixel adjacent
 * to an unselected one, mapped to UV by \a uv_cell (receives the pixel-cell corner index, which
 * may be one past the mask edge). Scans the [scan_min, scan_max) pixel window, \a stride rows at
 * a time, in parallel — so the segment order is arbitrary and every pair is independent.
 *
 * \param data: binary mask (single-channel float), \a size pixels.
 */
void image_select_mask_boundary_segments(const float *data,
                                         int2 size,
                                         int2 scan_min,
                                         int2 scan_max,
                                         const FunctionRef<float2(const int2 &)> uv_cell,
                                         int stride,
                                         Vector<float2> &r_segments);

/**
 * Draw \a texture on the surface: every mesh triangle overlapping the global-UV rect (\a uv_min,
 * \a uv_size) is drawn with texture coordinates mapped from its UVs onto that rect. Used by the
 * floating-selection previews, so the fragment pixels show where the commit will write them.
 */
void image_select_view3d_draw_uv_texture(const Object &ob,
                                         const ImageSelectView3DSurfaceMap &surface_map,
                                         gpu::Texture *texture,
                                         const float2 &uv_min,
                                         const float2 &uv_size);

/**
 * The general form of #image_select_view3d_draw_uv_texture for a *transformed* fragment: the
 * triangle filter runs on the \a dest rect (global UV), and each triangle's texture coordinate
 * comes from \a dest_uv_to_texco applied to its destination-space UV, so an affine-warped
 * fragment (the transform tool) samples its source texture through the inverse transform.
 */
void image_select_view3d_draw_uv_texture_ext(
    const Object &ob,
    const ImageSelectView3DSurfaceMap &surface_map,
    gpu::Texture *texture,
    const float2 &dest_rect_min,
    const float2 &dest_rect_max,
    const FunctionRef<float2(const float2 &dest_uv)> &dest_uv_to_texco);

/**
 * Free a lazy preview-texture table (one row per target, one slot per fragment), making the GPU
 * context current when the caller has none (a Viewport freed while a session floats, file load,
 * exit). Shared by the floating tools that keep GPU copies of their fragment display buffers.
 */
void image_select_view3d_preview_textures_free(Vector<Vector<gpu::Texture *>> &textures);

/** The single-texture variant of #image_select_view3d_preview_textures_free. */
void image_select_view3d_preview_texture_free(gpu::Texture *&texture);

/** Draw UV polylines as dashed lines projected onto the surface through \a surface_map. */
void image_select_view3d_draw_uv_outlines(const Object &ob,
                                          const ImageSelectView3DSurfaceMap &surface_map,
                                          const Span<Vector<float2>> uv_polylines,
                                          const float color[4],
                                          float dash_width);

/**
 * The permanent 3D Viewport overlay draw callback: the marching-ants outline of the selection
 * mask projected onto the surface. Registered once per VIEW_3D window region; cheap no-op when
 * there is no selection.
 */
void image_select_view3d_region_draw(const bContext *C, ARegion *region, void *arg);

/** Release the overlay caches (called from the Viewport space-data free hook). */
void image_select_view3d_overlay_free();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

/* Selection edits. */
void PAINT_OT_image_select_view3d_all(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_none(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_invert(wmOperatorType *ot);

/* Gesture selections. */
void PAINT_OT_image_select_view3d_box(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_lasso(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_polyline(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_circle(wmOperatorType *ot);

/* Floating tools. */
void PAINT_OT_image_select_view3d_move(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_transform(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_warp(wmOperatorType *ot);

/* Floating tools, shared: dispatch through the live session's virtual #commit / #cancel /
 * #undo_step, whichever tool is running. */
void PAINT_OT_image_select_view3d_floating_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_floating_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_floating_undo_step(wmOperatorType *ot);

/* Floating tools, shared: cycle the on-surface fragment preview's canvas image. */
void PAINT_OT_image_select_view3d_preview_channel(wmOperatorType *ot);

/* Clipboard. */
void PAINT_OT_image_select_view3d_copy(wmOperatorType *ot);
void PAINT_OT_image_select_view3d_paste(wmOperatorType *ot);

/** \} */

}  // namespace blender::ed::sculpt_paint
