/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Drag-and-drop of images/textures onto brush texture-slot buttons.
 */

#pragma once

#include <memory>

namespace blender {

struct ARegion;
struct Brush;
struct Image;
struct ImBuf;
struct Main;
struct Tex;
struct bContext;
struct wmEvent;
namespace ui {
class DropTargetInterface;
}  // namespace ui

/* -------------------------------------------------------------------- */
/** \name Preview Feedback (interface_drop_image_feedback.cc)
 * \{ */

/**
 * Load and scale a thumbnail-sized preview of an image file, for drag feedback.
 * \return The scaled #ImBuf, or null on failure.
 */
ImBuf *DROP_IMAGE_load_and_scale_preview(const char *filepath, int max_size = 128);

/**
 * Load and scale a thumbnail-sized preview from an existing #Image data-block, for drag feedback.
 * \return The scaled #ImBuf, or null on failure.
 */
ImBuf *DROP_IMAGE_load_and_scale_preview_from_id(Image *image, int max_size = 128);

/**
 * Regenerate the preview/icon of \a tex and refresh the editors showing it. The `_smart` variant
 * additionally refreshes the 3D viewport when the active object is in texture paint mode.
 */
void DROP_IMAGE_update_texture_preview(bContext *C, Main *bmain, Tex *tex, bool force_update = false);
void DROP_IMAGE_update_texture_paint_preview(bContext *C, Main *bmain, Tex *tex, Brush *brush = nullptr);
void DROP_IMAGE_update_texture_preview_smart(bContext *C, Main *bmain, Tex *tex, bool force_update = false);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drop Target & Registration (interface_drop_image.cc)
 * \{ */

/**
 * Return a drop target for the brush texture slot under the cursor, or null if the button under
 * the cursor is not one of the active brush's texture slots. Used by the generic button drop
 * dispatch #blender::ui::region_but_find_drop_target_at().
 */
std::unique_ptr<ui::DropTargetInterface> brush_texture_slot_drop_target_get(bContext *C,
                                                                            const ARegion *region,
                                                                            const wmEvent *event);

/**
 * Register the brush texture-slot image/texture drop box in the "User Interface" drop-box map.
 */
void DROP_IMAGE_register_dropboxes();

/** \} */

}  // namespace blender
