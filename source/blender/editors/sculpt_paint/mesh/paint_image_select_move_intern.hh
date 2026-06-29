/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Declarations for the move / copy-paste part of the Image Paint selection system.
 *
 * NOTE: This header is included at the end of #paint_image_select_intern.hh, which provides the
 * shared types it relies on (#SelectionTileFragment, #ImageSelectMoveState, etc.). Include
 * `paint_image_select_intern.hh` rather than this file directly.
 */

#pragma once

namespace blender {

void image_select_move_commit(bContext *C, ImageSelectMoveState *state);
void image_select_move_state_free(ImageSelectMoveState *state);

bool image_select_move_cursor_in_fragment(const ImageSelectMoveState *state,
                                          const ARegion *region,
                                          const wmEvent *event,
                                          Image *ima);
bool image_select_move_delegate_to_move_operator(bContext *C, const wmEvent *event);
ImBuf *image_select_move_extract(wmOperator *op,
                                 Image *ima,
                                 ImageUser *iuser,
                                 const SpaceImage *sima,
                                 int tile_number,
                                 int2 *r_origin_px,
                                 int2 *r_size_px,
                                 ImBuf **r_fragment_mask);
bool image_select_extract_per_tile(wmOperator *op,
                                   Image *ima,
                                   const ImageUser &base_iuser,
                                   Vector<SelectionTileFragment> *r_fragments);

bool image_select_move_is_floating(bContext *C);
bool image_select_move_is_floating_in_space(const SpaceImage *sima);
wmOperatorStatus image_select_move_convert_to_transform(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event);

void PAINT_OT_image_select_move(wmOperatorType *ot);
void PAINT_OT_image_select_move_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_move_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_move_undo_step(wmOperatorType *ot);
void PAINT_OT_image_select_copy(wmOperatorType *ot);
void PAINT_OT_image_select_paste(wmOperatorType *ot);

} /* namespace blender */
