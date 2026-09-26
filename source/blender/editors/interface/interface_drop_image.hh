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

#include "DNA_ID_enums.h" /* ID_Type */

namespace blender {

struct ARegion;
struct Brush;
struct ID;
struct Image;
struct ImBuf;
struct Main;
struct PointerRNA;
struct PropertyRNA;
struct Tex;
struct bContext;
struct wmDragAssetListItem;
struct wmEvent;
namespace ui {
class DropTargetInterface;
}  // namespace ui

/* -------------------------------------------------------------------- */
/** \name Asset List Drag Helpers (interface_drop_image.cc)
 * \{ */

/**
 * The ID type an Asset Browser multi-select drag item resolves to, without importing or resolving
 * anything. External (library) items are typed by their asset representation, local items by the
 * data-block itself.
 */
ID_Type drag_asset_list_item_idtype(const wmDragAssetListItem &item);

/** \} */

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

/**
 * Refresh the previews of several textures at once (multi-image drops adding many slots): kills
 * running preview jobs once, marks every texture's preview for regeneration and starts its
 * asynchronous render, then refreshes the editors once. Unlike calling
 * #DROP_IMAGE_update_texture_preview per texture, the per-texture job-kill does not cancel the
 * previous texture's render job and dependency relations are not rebuilt once per image.
 */
void DROP_IMAGE_update_textures_preview_batch(bContext *C,
                                              Main *bmain,
                                              const Span<Tex *> textures);

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
 * Return a drop target for the brush material source slot under the cursor, or null if the button
 * under the cursor is not the active brush's source material slot. Used by the generic button drop
 * dispatch #blender::ui::region_but_find_drop_target_at().
 */
std::unique_ptr<ui::DropTargetInterface> brush_material_slot_drop_target_get(
    bContext *C, const ARegion *region, const wmEvent *event);

/**
 * Return a drop target for a specialized Image #template_ID_browser control under the cursor, or
 * null if it is not one. Used by the generic button drop dispatch.
 */
std::unique_ptr<ui::DropTargetInterface> image_id_browser_drop_target_get(bContext *C,
                                                                         const ARegion *region,
                                                                         const wmEvent *event);

/**
 * Return a drop target for a Curve Patch texture-list control under the cursor (the
 * #SCULPT_UL_curve_patch_textures list: dropping appends one slot per image, dropping on a row
 * replaces that slot's texture), or null if there is none. Used by the generic button drop
 * dispatch, after #image_id_browser_drop_target_get so the row's own browser buttons keep their
 * target.
 */
std::unique_ptr<ui::DropTargetInterface> curve_patch_texture_list_drop_target_get(
    bContext *C, const ARegion *region, const wmEvent *event);

/**
 * Register the brush texture-slot image/texture drop box in the "User Interface" drop-box map.
 */
void DROP_IMAGE_register_dropboxes();

/** \} */

}  // namespace blender
