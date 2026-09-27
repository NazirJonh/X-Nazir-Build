/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Image Editor Vector session lifetime and its 2D #VectorEditHost target; the gestures live in
 * #ShapeVectorEditor. */

#pragma once

#include <memory>

#include "paint_image_select_floating.hh"
#include "../paint_shape.hh"
#include "../paint_vector_editor.hh"

namespace blender {
struct SpaceImage;
struct bContext;
}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

struct ImageShapeVectorState;

std::unique_ptr<VectorEditHost> image_shape_vector_host_create(bContext &C,
                                                               ImageShapeVectorState &session,
                                                               float tolerance_px);
ImageShapeVectorState *image_shape_vector_state_get(const SpaceImage *sima);
/** The live session's edit document in \a sima, or null when no session is floating. */
ShapeEditSession *image_shape_vector_edit_get(const SpaceImage *sima);
bool image_shape_vector_is_floating_in_space(const SpaceImage *sima);
/** Runtime "Transform" mode of the session in \a sima (shows the cage for Polygon/Star/Arc). */
bool image_shape_vector_transform_mode_get(const SpaceImage *sima);
void image_shape_vector_transform_mode_toggle(SpaceImage *sima);
void image_shape_vector_session_begin(bContext *C,
                                      SpaceImage *sima,
                                      PaintShape shape,
                                      const CanvasTile &tile,
                                      const ShapeStyle &style);
void image_shape_vector_session_end_for_takeover(bContext *C, SpaceImage *sima);
void image_shape_vector_session_cancel(bContext *C, SpaceImage *sima);
void image_shape_vector_state_free(PaintSelectFloatingSession *session);
bool image_shape_vector_modal_active(const ImageShapeVectorState *state);
void image_shape_vector_modal_set_active(ImageShapeVectorState *state, bool active);

}  // namespace blender::ed::sculpt_paint::shape
