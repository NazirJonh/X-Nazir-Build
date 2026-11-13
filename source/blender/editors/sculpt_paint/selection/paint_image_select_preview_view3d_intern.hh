/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The state of the 3D Viewport preview bridge (`paint_image_select_preview_view3d.cc`), shared with
 * the Decal module (`paint_image_select_editor_decal.cc`), which reads the live decal, the surface
 * map and the stamp textures of a session's preview. Not for other consumers.
 */

#pragma once

#include "BLI_vector.hh"

#include "paint_image_select_editor_decal.hh"
#include "paint_image_select_preview_view3d.hh"
#include "paint_image_select_view3d.hh"

struct ARegionType;

namespace blender::ed::sculpt_paint {

struct ImageSelect3DPreview {
  PaintSelectFloatingSession *session = nullptr;
  ARegionType *region_type = nullptr;
  void *handle = nullptr;
  /** The canvas maps the preview draws, parallel to #ImageSelect3DPreviewFrame::fragments. */
  Vector<ImagePaintSelectionTarget> targets;
  ImageSelect3DPreviewFrameFn frame_get = nullptr;
  /** The attach arguments, kept so a live mode switch can rebuild the preview the same way. */
  bool primary_target_only = false;
  ImageSelect3DPreviewFrame frame;
  ImageSelectView3DSurfaceMap surface_map;
  /** GPU copies of the fragments' display buffers (the stamps in Decal mode), created lazily by the
   * draw callback, where a GPU context is current. */
  Vector<Vector<gpu::Texture *>> textures;
  /** Neutral 1x1 stand-ins of the PBR composite, one per #PBRPreviewChannel. */
  gpu::Texture *pbr_default_textures[PBR_PREVIEW_CHANNEL_NUM] = {nullptr};
  bool decal_mode = false;
  ImageSelectEditorDecal decal;
  /** GPU copies of the decal stamps for the Image Editor 2D drawing (parallel to #targets), kept
   * apart from #textures because the 2D region may draw in another GPU context. */
  Vector<gpu::Texture *> stamp_textures_2d;

  ~ImageSelect3DPreview();
};

}  // namespace blender::ed::sculpt_paint
