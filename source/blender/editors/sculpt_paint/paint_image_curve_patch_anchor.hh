/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * The Curve Patch anchor gesture as an #ImageStrokeMethodHook.
 */

#include <memory>

#include "paint_image_stroke_hook.hh"

namespace blender::ed::sculpt_paint {

/**
 * Build the hook that records a `BRUSH_STROKE_CURVE_PATCH` anchor gesture and, when it ends,
 * opens an #ImageCurvePatchSession around the curve it produced.
 *
 * The stroke that owns the hook must not paint: #ImageStrokeMethodHook::suppresses_dabs is true
 * for the whole gesture (see there for why rolling the dabs back afterwards is not equivalent).
 *
 * Launching the modal editor is deliberately NOT part of this. The hook lives inside the stroke,
 * and a stroke cannot know whether its caller wants an interactive editor -- the operator's own
 * exit paths call `PAINT_OT_image_curve_patch_edit`, mirroring the same split the 3D side makes
 * in `curve_patch_start_from_anchor()`.
 */
std::unique_ptr<ImageStrokeMethodHook> image_curve_patch_anchor_hook_create();

}  // namespace blender::ed::sculpt_paint
