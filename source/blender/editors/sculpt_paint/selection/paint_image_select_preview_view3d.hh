/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The 3D Viewport preview bridge of the Image Editor's floating Transform / Move sessions.
 *
 * Those sessions lift the fragment off the canvas and draw the floating pixels only in the Image
 * Editor, so a 3D Viewport reading the canvas sees a hole. The bridge registers a POST_VIEW draw
 * callback on the View3D region type that draws the floating fragments on the surface of the active
 * object when that object uses the session's canvas (nothing is drawn, and nothing fails, when no
 * object does). It is tool-agnostic: a session only hands over a #ImageSelect3DPreviewFrameFn that
 * reports its fragments and its current UV mapping.
 *
 * Two content mappings are drawn (and committed) the way the 3D Viewport's Transform tool does:
 * - UV: the UV affine, the PBR composite of the selected maps through
 *   #image_select_view3d_draw_pbr_composite.
 * - Decal: the content follows the surface (see `paint_image_select_surface_decal.hh`).
 */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "paint_image_select_editor_decal.hh"
#include "paint_image_select_floating.hh"
#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_surface_decal.hh"

struct ARegion;
struct Image;
struct ReportList;
struct bContext;

namespace blender::ed::sculpt_paint {

struct ImageSelect3DPreviewFrame {
  /** Fragment sets parallel to the preview's targets: shallow views of the session's fragments
   * (the session keeps the ownership), rebuilt on every call. */
  Vector<Vector<SelectionTileFragment>> fragments;
  ImageSelectEditorMapping mapping;
};

/**
 * Fill \a r_frame from the live \a session (the fragment set of every entry of \a targets, an empty
 * set for a target the session does not lift). False when the session cannot report one.
 */
using ImageSelect3DPreviewFrameFn = bool (*)(const PaintSelectFloatingSession &session,
                                             Span<ImagePaintSelectionTarget> targets,
                                             ImageSelect3DPreviewFrame &r_frame);

/**
 * Register the preview of \a session on the View3D region type (replacing a previous one). The
 * canvas maps it draws are the session image's selection targets (only the session image itself
 * with \a primary_target_only: Move lifts one image).
 *
 * \param want_decal: the mode the tool settings ask for. A Decal that cannot be opened (no mesh
 * object, no UV map, no surface under the fragment) falls back to UV with a warning in \a reports.
 * \return true when the Decal mapping is live; the session then commits through
 * #image_select_editor_decal_commit.
 */
bool image_select_preview_view3d_attach(bContext *C,
                                        PaintSelectFloatingSession &session,
                                        ImageSelect3DPreviewFrameFn frame_get,
                                        bool primary_target_only,
                                        bool want_decal,
                                        ReportList *reports);

/**
 * Switch the live preview of \a session between UV and Decal while it floats, keeping the session's
 * gesture (the mapping is read from the session, not from the preview). Rebuilds the preview
 * through #image_select_preview_view3d_attach with the arguments it was attached with; a Decal that
 * cannot be opened leaves the UV preview (diagnostic on the console only).
 * \param r_decal_active: whether the Decal mapping is live afterwards (the session's own
 * "use decal" flag must follow it).
 * \return false when the session has no preview, or it could not be rebuilt (then none is left).
 */
bool image_select_preview_view3d_mode_switch(bContext *C,
                                             PaintSelectFloatingSession &session,
                                             bool want_decal,
                                             bool &r_decal_active);

/** Remove the callback and free the preview's GPU resources. Idempotent. */
void image_select_preview_view3d_detach(PaintSelectFloatingSession &session);

/** Tag the 3D Viewports for redraw after a drag step moved the fragment; no-op without a preview. */
void image_select_preview_view3d_tag_redraw(const bContext *C,
                                            const PaintSelectFloatingSession &session);

/** The canvas maps a session of \a sima lifts (see #image_select_preview_view3d_attach). */
Vector<ImagePaintSelectionTarget> image_select_editor_targets_get(const bContext *C,
                                                                  const SpaceImage *sima,
                                                                  bool primary_target_only);

/** Whether the tool settings ask for the Decal mapping. */
bool image_select_editor_decal_requested(const bContext *C);

}  // namespace blender::ed::sculpt_paint
