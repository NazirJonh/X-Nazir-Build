/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The surface-decal core of Transform Selection: stamps built from lifted fragments, the unfolded
 * surface patch that carries them, the POST_VIEW preview and the bake into the canvas targets.
 * Shared by the 3D Viewport tool (`paint_image_select_transform_view3d_decal.cc` owns its gestures)
 * and by the Image Editor's Transform / Move sessions (`paint_image_select_preview_view3d.cc`).
 */

#pragma once

#include <cmath>

#include "BLI_function_ref.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_surface_unfold.hh"
#include "paint_image_select_view3d.hh"

struct ARegion;
struct Depsgraph;
struct Image;
struct ImBuf;
struct Object;
struct bContext;

namespace blender::ed::sculpt_paint {

namespace image_select_v3d {

/** Map a UV point through the forward affine transform. */
inline float2 transform_uv(const float2x2 &linear,
                           const float2 &pivot_uv,
                           const float2 &translation_uv,
                           const float2 &uv)
{
  return pivot_uv + linear * (uv - pivot_uv) + translation_uv;
}

/** Rotation by \a angle in the plane (columns, like the rest of the plane math). */
inline float2x2 plane_rotation(const float angle)
{
  const float s = std::sin(angle);
  const float c = std::cos(angle);
  return float2x2({c, s}, {-s, c});
}

}  // namespace image_select_v3d

/**
 * Decal session data: the stamps the lifted fragments were re-sampled into, the surface patch they
 * are carried over, and the surface frame that places the stamp on the patch. The frame is authored
 * on the surface (a center UV, a roll relative to the sampled tangent, and scale factors on the
 * frame axes) rather than as a UV affine, and the patch is unfolded from the mesh's triangles (see
 * #surface_unfold_build), which is what lets the content cross seams, island borders and cube edges
 * unbroken. Nothing of it lives in the material.
 */
struct SurfaceDecalSession {
  /** One stamp per target (parallel to the session's targets, null where none could be built):
   * float RGBA, straight alpha, in the canvas's own encoding. Owned, freed by
   * #surface_decal_release. Not Images: they never reach the Main or the file. */
  Vector<ImBuf *> stamps;
  /** The frame center, as the surface UV it is anchored to. */
  float2 center_uv = float2(0.0f);
  /** Orientation of the frame X from the sampled tangent (the tangent is kept continuous by the
   * session, so the stamp does not spin across a curved surface). */
  float roll = 0.0f;
  /** Scale on the frame axes, on top of #base_size. */
  float2 scale = float2(1.0f);
  /** The stamp extents in object units, the frame extents the scale factors apply to. */
  float2 base_size = float2(0.0f);
  /** The stamp center in the patch plane (object units, origin at the patch seed). Zero while the
   * center lies on the surface; non-zero when the Image Editor moved the center into a UV gap and
   * the patch stays seeded at the last valid point. */
  float2 plane_offset = float2(0.0f);
  /** Triangle adjacency of the session's surface map, built on the first patch. */
  SurfaceUnfoldAdjacency adjacency;
  /** The mesh triangles around the frame center flattened into the plane of the center's triangle
   * (origin at the center, X along #patch_tangent). Rebuilt when the center moves or the stamp
   * outgrows it; a roll or a scale only re-maps it onto the stamp. */
  SurfaceUnfoldPatch patch;
  /** What #patch was built from, to tell when it is stale. */
  int patch_seed_tri = -1;
  float3 patch_seed_co = float3(0.0f);
  float3 patch_tangent = float3(0.0f);
};

/** Free the stamps and drop the patch. Idempotent; safe on every exit path. */
void surface_decal_release(SurfaceDecalSession &decal);

/** The cage a stamp is cut from: a rect in plane px, oriented by #frame_angle, around #anchor_uv. */
struct SurfaceDecalCage {
  /** Source UV of the plane origin. */
  float2 anchor_uv = float2(0.0f);
  /** Plane px -> UV: `uv = anchor_uv + plane_uv_jacobian * (R(frame_angle) * plane_px)`. */
  float2x2 plane_uv_jacobian = float2x2::identity();
  float2 plane_min = float2(0.0f);
  float2 plane_max = float2(0.0f);
  float frame_angle = 0.0f;
};

/**
 * The stamp of one target: \a fragments re-sampled from their UV space into \a cage (stamp texel
 * (i, j) reads the source texel the cage coordinate maps to), the selection coverage times the
 * texel's own alpha as the straight alpha, a Normal target's vectors rewritten into the cage basis.
 * \return null when nothing could be built (no fragments, a degenerate Jacobian).
 */
ImBuf *surface_decal_stamp_build(const SurfaceDecalCage &cage,
                                 const ImagePaintSelectionTarget &target,
                                 Span<SelectionTileFragment> fragments);

/** A frame sampled on the surface at a UV, independent of any View3D. */
struct SurfaceDecalFrame {
  float3 co = float3(0.0f);
  float3 normal = float3(0.0f, 0.0f, 1.0f);
  float3 tangent = float3(1.0f, 0.0f, 0.0f);
  float3 bitangent = float3(0.0f, 1.0f, 0.0f);
  /** The surface corner triangle the UV resolved to. */
  int tri = -1;
  /** Plane (tangent, bitangent; object units) -> UV Jacobian of #tri. */
  float2x2 plane_uv_jacobian = float2x2::identity();
  bool jacobian_valid = false;
};

/**
 * Sample the surface at \a uv: the point, the normal, a tangent frame close to \a prev_tangent and
 * the triangle's plane -> UV Jacobian. Needs no raycast and no View3D, so a session without a
 * Viewport (the Image Editor's) can place a decal. False when \a uv is off the surface.
 */
bool surface_decal_frame_at_uv(const Object &ob,
                               const Depsgraph &depsgraph,
                               const ImageSelectView3DSurfaceMap &surface_map,
                               const float2 &uv,
                               const float3 &prev_tangent,
                               SurfaceDecalFrame &r_frame);

/**
 * Make the unfolded patch fit the current frame (seeded at \a seed_tri / \a seed_co with the
 * X axis along \a seed_tangent): rebuilt only when the seed moved or the stamp outgrew the patch.
 * \return false when no patch can be built (no surface under the seed); the previous patch, if any,
 * is kept.
 */
bool surface_decal_patch_ensure(const bContext *C,
                                SurfaceDecalSession &decal,
                                Object *ob,
                                const ImageSelectView3DSurfaceMap &surface_map,
                                int seed_tri,
                                const float3 &seed_co,
                                const float3 &seed_tangent);

/**
 * The POST_VIEW preview: the patch triangles at their surface positions, textured with the stamp of
 * \a preview_target (or the PBR composite of all of them when it is -1). Draws nothing into the
 * material; needs the GPU context the draw callback runs in. \a preview_textures and
 * \a pbr_default_textures are the caller's lazily filled GPU caches.
 */
void surface_decal_draw_preview(const ARegion *region,
                                const SurfaceDecalSession &decal,
                                const Object &ob,
                                const ImageSelectView3DSurfaceMap &surface_map,
                                Span<ImagePaintSelectionTarget> targets,
                                Vector<Vector<gpu::Texture *>> &preview_textures,
                                gpu::Texture *pbr_default_textures[PBR_PREVIEW_CHANNEL_NUM],
                                int preview_target);

/**
 * The patch as the bake sees it: three vertices per triangle, the corner UV in the canvas (global
 * UV, UDIM included) and the stamp coordinate of the same vertex (#surface_decal_stamp_uv). For
 * drawing the patch in the Image Editor.
 */
void surface_decal_patch_uv_vertices(const SurfaceDecalSession &decal,
                                     const ImageSelectView3DSurfaceMap &surface_map,
                                     Vector<float2> &r_uv,
                                     Vector<float2> &r_stamp_uv);

/**
 * Bake every target's stamp through the patch into its canvas, in ONE image undo step: the source
 * regions are cut like the UV commit's, the patch triangles' UV footprints composite the stamp over
 * the canvas (gutter, per-triangle normal rotation). \a targets and \a fragments are parallel;
 * entries without fragments or whose image \a image_is_alive refuses are skipped.
 */
void surface_decal_commit(bContext *C,
                          const char *undo_label,
                          const SurfaceDecalSession &decal,
                          const ImageSelectView3DSurfaceMap &surface_map,
                          Span<ImagePaintSelectionTarget> targets,
                          Span<Vector<SelectionTileFragment>> fragments,
                          FunctionRef<bool(const Image *)> image_is_alive);

}  // namespace blender::ed::sculpt_paint
