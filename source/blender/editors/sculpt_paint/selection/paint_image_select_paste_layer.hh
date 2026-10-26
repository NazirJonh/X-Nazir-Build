/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Paste Selection into the 3D Viewport canvas: into a new layer of a Material's paint layer stack
 * (the Selection Paste setting's New Layer mode) or into the current layer.
 */

#pragma once

#include "BLI_vector.hh"

struct ReportList;
struct bContext;
struct Object;

namespace blender {
struct ImageSelectClipboard;
}  // namespace blender

namespace blender::ed::sculpt_paint {

/** True while the Selection Paste setting asks for the New Layer mode. */
bool image_select_paste_new_layer_enabled(bContext *C);

/**
 * Paste the clipboard fragments onto \a ob's canvas. The previous selection is cleared, the
 * Transform tool becomes active and a floating Transform session holds the fragments at the view
 * center, so the user places them before they are written.
 *
 * In the New Layer mode (a Material canvas on layer content) the fragments target a new Paint row
 * above the active layer, with one transparent map per pasted channel. Otherwise they target the
 * canvas image(s) of the current layer, each clipboard group paired with the target of the same
 * channel (a Material canvas) or the same image.
 *
 * \return true when the paste handled the clipboard (a new layer counts even when placing failed,
 * its failure is reported); false when nothing of the clipboard fits this canvas, with a warning.
 */
bool image_select_paste_clipboard(bContext *C,
                                  ReportList *reports,
                                  Object &ob,
                                  const ImageSelectClipboard &clipboard);

}  // namespace blender::ed::sculpt_paint
