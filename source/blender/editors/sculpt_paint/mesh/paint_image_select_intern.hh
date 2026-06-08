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
#include "IMB_imbuf.hh"
#include "BLI_vector.hh"

struct ImBuf;
struct ARegion;
struct Image;
struct ImageUser;
struct SpaceImage;
struct bContext;
struct wmEvent;
struct wmOperator;
struct wmOperatorType;
struct wmGizmo;
struct wmGizmoGroupType;

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

/* One compact fragment per UDIM tile that has an active selection.
 * origin_px and size_px are always in tile-local pixel coordinates (>= 0). */
struct SelectionTileFragment {
  ImBuf *fragment_ibuf = nullptr;
  ImBuf *fragment_mask_ibuf = nullptr;
  /* 4-channel RGBA float preview with mask baked into alpha (GPU draw only). */
  ImBuf *fragment_display_ibuf = nullptr;
  int2 origin_px = {0, 0};
  int2 size_px = {0, 0};
  int2 tile_size_px = {1, 1};
  int tile_number = 1001;
};

inline void selection_tile_fragment_free(SelectionTileFragment &frag)
{
  if (frag.fragment_ibuf) {
    IMB_freeImBuf(frag.fragment_ibuf);
    frag.fragment_ibuf = nullptr;
  }
  if (frag.fragment_mask_ibuf) {
    IMB_freeImBuf(frag.fragment_mask_ibuf);
    frag.fragment_mask_ibuf = nullptr;
  }
  if (frag.fragment_display_ibuf) {
    IMB_freeImBuf(frag.fragment_display_ibuf);
    frag.fragment_display_ibuf = nullptr;
  }
}

inline void selection_tile_fragments_free(Vector<SelectionTileFragment> &fragments)
{
  for (SelectionTileFragment &frag : fragments) {
    selection_tile_fragment_free(frag);
  }
  fragments.clear();
}

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
 * Extract one #SelectionTileFragment per UDIM tile that has a non-empty selection mask.
 * Each fragment uses tile-local pixel coordinates (origin_px >= 0 always).
 * Appends to \a r_fragments; does not clear it first.
 * Returns true if at least one fragment was extracted.
 */
bool image_select_extract_per_tile(wmOperator *op,
                                   Image *ima,
                                   const ImageUser &base_iuser,
                                   Vector<SelectionTileFragment> *r_fragments);
/**
 * Build a 4-channel RGBA float ImBuf for GPU preview: RGB from \a src, alpha from \a mask.
 */
ImBuf *image_select_make_display_ibuf(const ImBuf *src, const ImBuf *mask);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform gizmo integration (opaque state API)
 * \{ */

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

bool image_select_move_is_floating(bContext *C);
/** Floating move in \a sima (not another Image Editor space). */
bool image_select_move_is_floating_in_space(const SpaceImage *sima);
/**
 * Replace floating move-selection with transform mode at the current offset.
 * Returns #OPERATOR_RUNNING_MODAL on success.
 */
wmOperatorStatus image_select_move_convert_to_transform(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event);

/**
 * Begin transform mode from pixels already lifted for move (no second lift / undo step).
 * Takes ownership of \a fragments (moved in).
 * \a uv_drag_offset is the accumulated UV-space drag from the move phase.
 * \a ref_tile_number is the first fragment's tile, used only to seed the ImageUser.
 */
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

bool ED_image_paint_select_is_transforming(SpaceImage *sima);
void ED_image_paint_select_translation_get(SpaceImage *sima, float r_translation[2]);
void ED_image_paint_select_translation_set(SpaceImage *sima, const float translation[2]);
float ED_image_paint_select_rotation_get(SpaceImage *sima);
void ED_image_paint_select_rotation_set(SpaceImage *sima, float rotation);
void ED_image_paint_select_scale_get(SpaceImage *sima, float r_scale[2]);
void ED_image_paint_select_scale_set(SpaceImage *sima, const float scale[2]);

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

void ED_image_paint_select_transform_gizmo_setup(wmGizmoGroupType *gzgt);

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
