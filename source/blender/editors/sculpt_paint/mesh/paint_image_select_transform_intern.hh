/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Declarations for the transform (scale / rotate / anchor) and cage-gizmo part of the Image Paint
 * selection system.
 *
 * NOTE: This header is included at the end of #paint_image_select_intern.hh, which provides the
 * shared types it relies on (#SelectionTileFragment, #ImageSelectTransformState, etc.). Include
 * `paint_image_select_intern.hh` rather than this file directly.
 */

#pragma once

namespace blender {

enum class ImageSelectTransformHandleType {
  None = 0,
  Move,
  Rotate,
  Anchor,
  C0,
  C1,
  C2,
  C3,
  MBottom,
  MRight,
  MTop,
  MLeft,
};

struct ImageSelectTransformGizmoMatrices {
  float matrix_space[4][4];
  float matrix_basis[4][4];
  float matrix_offset[4][4];
  float cage_center_uv[3];
  float anchor_screen[3];
};

void image_select_transform_state_free(ImageSelectTransformState *state);

/** Tag-dispatched entry point of #image_select_floating_session_free. */
void image_select_transform_session_free(PaintSelectFloatingSession *session);

/**
 * Commit and free \a sima's floating transform session, if it has one. No-op otherwise.
 * Called through #image_select_floating_sessions_end; see that function for the semantics.
 */
void image_select_transform_session_end_for_takeover(bContext *C, SpaceImage *sima);

wmOperatorStatus image_select_transform_adopt_move_state(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event,
                                                         SpaceImage *sima,
                                                         Vector<SelectionTileFragment> &&fragments,
                                                         float2 uv_drag_offset,
                                                         int ref_tile_number,
                                                         const ImageUser &iuser,
                                                         bool undo_begun,
                                                         bool proportional);
bool image_select_transform_is_floating_in_space(const SpaceImage *sima);
ImageSelectTransformState *image_select_transform_state_get(SpaceImage *sima);

ImageSelectTransformHandleType image_select_transform_cage_part_to_handle_type(int cage_part);
void image_select_transform_begin_drag(ImageSelectTransformState *state,
                                       const wmEvent *event,
                                       ImageSelectTransformHandleType handle);
void image_select_transform_end_drag(ImageSelectTransformState *state);
void image_select_transform_apply_handle(bContext *C,
                                         ImageSelectTransformState *state,
                                         const wmEvent *event,
                                         ImageSelectTransformHandleType handle,
                                         ARegion *region);
bool image_select_transform_calc_gizmo_matrices(const bContext *C,
                                                const ImageSelectTransformState *state,
                                                ImageSelectTransformGizmoMatrices *r_mats);
bool image_select_transform_has_active_handle(const ImageSelectTransformState *state);
void image_select_transform_gizmo_refresh_tweak(const bContext *C,
                                                wmGizmo *gz_cage,
                                                wmGizmo *gz_anchor,
                                                bool *r_was_modal_tweak);

void PAINT_OT_image_select_transform(wmOperatorType *ot);
void PAINT_OT_image_select_transform_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_transform_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_transform_drag(wmOperatorType *ot);

} /* namespace blender */
