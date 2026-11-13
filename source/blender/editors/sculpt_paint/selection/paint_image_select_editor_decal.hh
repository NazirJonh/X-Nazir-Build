/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Decal frame of the Image Editor's floating Transform / Move sessions: the surface decal that
 * follows the session's UV gesture, its commit into the canvas, the handoff that carries it across
 * the Transform confirm, and its 2D drawing.
 *
 * The 3D Viewport preview bridge (`paint_image_select_preview_view3d.hh`) owns the live decal of a
 * session; this module derives it from the gesture and bakes it. This header must not include the
 * bridge header (the bridge includes this one).
 */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "paint_image_select_floating.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_surface_decal.hh"

struct ARegion;
struct Image;
struct Object;
struct SpaceImage;
struct bContext;

namespace blender::ed::sculpt_paint {

/**
 * The forward UV mapping of an Image Editor session, `dest = pivot + linear * (src - pivot) +
 * translation`, plus the factors the decal frame reads.
 */
struct ImageSelectEditorMapping {
  float2x2 linear = float2x2::identity();
  float2 pivot = float2(0.0f);
  float2 translation = float2(0.0f);
  /** Rotation of the gesture (radians, counter-clockwise in UV space). */
  float rotation = 0.0f;
  /** Scale on the UV axes of the source, applied before the rotation. */
  float2 scale = float2(1.0f);
};

/**
 * The decal of an Image Editor session: the frame at the source cage center and the surface
 * session (patch, stamps) that carries the lifted fragments. The stamps are owned and freed by
 * #surface_decal_release.
 */
struct ImageSelectEditorDecal {
  Object *object = nullptr;
  /** The source cage center (UV): the point the frame starts on. */
  float2 source_center_uv = float2(0.0f);
  /** The last sampled frame tangent, so the stamp does not spin across a curved surface. */
  float3 tangent = float3(1.0f, 0.0f, 0.0f);
  /** +1 when the surface frame (tangent, bitangent) has the orientation of the UV axes there, -1
   * for a mirrored UV (a rotation then turns the other way on the surface). */
  float orientation = 1.0f;
  /** The last center UV that had a surface under it, with the inverse of that frame's plane -> UV
   * Jacobian: a center in a UV gap is extrapolated affinely from here into the patch plane. */
  float2 seed_uv = float2(0.0f);
  float2x2 seed_jacobian_inv = float2x2::identity();
  bool seed_valid = false;
  /** Whether the center is currently off the surface (for the transition diagnostics). */
  bool off_surface = false;
  SurfaceDecalSession session;
};

/**
 * The decal kept by #image_select_editor_decal_handoff_store, with the mapping it was shown at.
 * Owned by the Image Editor's runtime (#PaintSelectSession::editor_decal_handoff) and freed through
 * #image_select_editor_decal_handoff_release.
 */
struct ImageSelectEditorDecalHandoff {
  ImageSelectEditorDecal decal;
  ImageSelectEditorMapping mapping;
};

/**
 * Open the decal for the fragments of an Image Editor session: the frame at the center of the
 * source cage, the cage rect in that frame's plane, one stamp per target and the first patch.
 * False when the center has no surface or nothing could be built.
 */
bool image_select_editor_decal_begin(const bContext *C,
                                     Object &ob,
                                     const ImageSelectView3DSurfaceMap &surface_map,
                                     Span<ImagePaintSelectionTarget> targets,
                                     Span<Vector<SelectionTileFragment>> fragments,
                                     ImageSelectEditorDecal &r_decal);

/**
 * Move the decal frame to where \a mapping puts the source center. The roll and the scale always
 * follow; the patch is re-seeded only when the new center has a surface under it, otherwise the
 * last valid center and patch stay (the same re-seat rule as the 3D tool).
 * \return true when the center is on the surface and the patch was refreshed.
 */
bool image_select_editor_decal_update(const bContext *C,
                                      Object &ob,
                                      const ImageSelectView3DSurfaceMap &surface_map,
                                      ImageSelectEditorDecal &decal,
                                      const ImageSelectEditorMapping &mapping);

/**
 * Commit lifted fragments through the Decal mapping: the active mesh object's surface carries the
 * content from the source position to where \a mapping moved it, one image undo step. The decal
 * comes from the live preview of \a session, or from a matching handoff.
 * \return false when neither is available or no decal could be opened (nothing was written; the
 * caller commits through the UV affine instead).
 */
bool image_select_editor_decal_commit(bContext *C,
                                      const char *undo_label,
                                      const PaintSelectFloatingSession *session,
                                      Span<ImagePaintSelectionTarget> targets,
                                      Span<Vector<SelectionTileFragment>> fragments,
                                      const ImageSelectEditorMapping &mapping);

/**
 * Whether \a mapping keeps the decal's center on the surface. True when the session has no Decal
 * preview, so a UV session is never constrained. The drag handlers use it to stop the cage and the
 * gizmo where the fragment itself can no longer follow (a UV gap), so the two stay in step.
 */
bool image_select_editor_decal_mapping_valid(const bContext *C,
                                             const PaintSelectFloatingSession &session,
                                             const ImageSelectEditorMapping &mapping);

/**
 * Keep the decal \a session's preview shows past the end of the session, for the commit that
 * follows it. The Transform confirm ends the session and then re-runs the operator through exec;
 * the next #image_select_editor_decal_commit with the same \a mapping bakes the kept decal. It is
 * stored in the runtime of the session's Image Editor, replacing an earlier handoff that was never
 * consumed.
 */
void image_select_editor_decal_handoff_store(const PaintSelectFloatingSession &session,
                                             const ImageSelectEditorMapping &mapping);

/**
 * Free the handoff of \a sima (its stamps included). No-op without one. Called when the handoff is
 * consumed or dropped, when a new session starts and when the Image Editor's sessions are cancelled
 * or the editor is freed, so no stamp outlives its editor.
 */
void image_select_editor_decal_handoff_release(SpaceImage *sima);

/** Read-only view of the live decal of a session's preview, for the Image Editor 2D drawing. */
struct ImageSelectEditorDecalView {
  /** Patch (#SurfaceDecalSession::patch), stamps and frame of the current mapping. */
  const SurfaceDecalSession *decal = nullptr;
  const ImageSelectView3DSurfaceMap *surface_map = nullptr;
  Span<ImagePaintSelectionTarget> targets;
};

/**
 * The live decal of \a session (refreshed to its current mapping), the exact patch the commit will
 * bake. False when the session has no Decal mapping or no patch (the 2D view then draws the UV
 * quad).
 */
bool image_select_editor_decal_patch_get(const bContext *C,
                                         const PaintSelectFloatingSession &session,
                                         ImageSelectEditorDecalView &r_view);

/**
 * Draw the decal patch in the Image Editor: each triangle at its own UV (through \a uv_to_region)
 * textured with the stamp of the session's primary target. Returns false when nothing was drawn.
 */
bool image_select_editor_decal_draw_2d(const bContext *C,
                                       const PaintSelectFloatingSession &session,
                                       const ARegion *region,
                                       const Image *image);

}  // namespace blender::ed::sculpt_paint
