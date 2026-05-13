/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal shared declarations for the Image Paint selection system.
 * Consumed by paint_image_select_mask.cc, paint_image_select_move.cc, and
 * paint_image_select_transform.cc.
 */

#pragma once

#include "BKE_image.hh"

#include "BLI_math_vector_types.hh"

struct ImBuf;
struct ARegion;
struct Image;
struct ImageUser;
struct SpaceImage;
struct bContext;
struct wmEvent;
struct wmOperator;
struct wmOperatorType;

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/* Alias for BKE threshold -- single source of truth in BKE_image.hh. */
constexpr float SELECTION_MASK_THRESHOLD = IMAGE_PAINT_SELECTION_MASK_THRESHOLD;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared types
 * \{ */

/* A lifted rectangular fragment of canvas pixels plus its selection mask.
 * Shared sub-object used by ImageSelectMoveState and ImageSelectTransformState. */
struct ImagePaintSelectionFragment {
  ImBuf *fragment_ibuf = nullptr;
  ImBuf *fragment_mask_ibuf = nullptr;
  /* 4-channel RGBA float copy with mask baked into alpha, used for GPU preview only. */
  ImBuf *fragment_display_ibuf = nullptr;
  int2 origin_px = {0, 0};
  int2 size_px = {0, 0};
  int tile_number = 0;
  bool has_mask = false;
};

/* Forward declarations -- full definitions remain in the implementation files. */
struct ImageSelectMoveState;
struct ImageSelectTransformState;

/* Per-Image-Editor floating-operation state, owned by SpaceImage_Runtime (Task 4). */
struct PaintSelectSession {
  ImageSelectMoveState *move = nullptr;
  ImageSelectTransformState *transform = nullptr;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifetime helpers
 * \{ */

/* Free any in-progress move/transform state owned by the session.
 * Safe to call with null pointers; does not free the session itself. */
void paint_select_session_free(PaintSelectSession &session);

void image_select_move_commit(bContext *C, ImageSelectMoveState *state);
void image_select_move_state_free(ImageSelectMoveState *state);
void image_select_transform_state_free(ImageSelectTransformState *state);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cross-module helpers (mask / move / transform)
 * \{ */

bool image_paint_selection_poll(bContext *C);
/**
 * Prefer \a preferred_tile if it has a mask; otherwise the first tile with a non-empty
 * selection (UDIM), or \a preferred_tile as fallback.
 */
int image_paint_selection_resolve_tile(Image *ima, const SpaceImage *sima, int preferred_tile);
bool image_select_move_cursor_in_fragment(const ImageSelectMoveState *state,
                                          const ARegion *region,
                                          const wmEvent *event,
                                          Image *ima);
/**
 * When a move-selection fragment is floating and the cursor is inside it, invoke
 * #PAINT_OT_image_select_move and return true. Otherwise return false.
 */
bool image_select_move_delegate_to_move_operator(bContext *C, const wmEvent *event);
ImBuf *image_select_move_extract(wmOperator *op,
                                 Image *ima,
                                 ImageUser *iuser,
                                 const SpaceImage *sima,
                                 int tile_number,
                                 int2 *r_origin_px,
                                 int2 *r_size_px,
                                 ImBuf **r_fragment_mask);
/**
 * Build a 4-channel RGBA float ImBuf for GPU preview: RGB from \a src, alpha from \a mask.
 */
ImBuf *image_select_make_display_ibuf(const ImBuf *src, const ImBuf *mask);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator registration prototypes
 * \{ */

void PAINT_OT_image_select_all(wmOperatorType *ot);
void PAINT_OT_image_select_none(wmOperatorType *ot);
void PAINT_OT_image_select_invert(wmOperatorType *ot);
void PAINT_OT_image_select_box(wmOperatorType *ot);
void PAINT_OT_image_select_lasso(wmOperatorType *ot);
void PAINT_OT_image_select_circle(wmOperatorType *ot);
void PAINT_OT_image_select_move(wmOperatorType *ot);
void PAINT_OT_image_select_move_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_move_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_move_undo_step(wmOperatorType *ot);
void PAINT_OT_image_select_copy(wmOperatorType *ot);
void PAINT_OT_image_select_paste(wmOperatorType *ot);
void PAINT_OT_image_select_transform(wmOperatorType *ot);
void PAINT_OT_image_select_transform_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_transform_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_transform_drag(wmOperatorType *ot);

/** \} */

} /* namespace blender */
