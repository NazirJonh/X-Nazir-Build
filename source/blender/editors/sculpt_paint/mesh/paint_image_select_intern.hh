/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal shared declarations for the Image Paint selection system.
 *
 * Flow (high level):
 * \code{.unparsed}
 *   select (mask.cc) --> runtime binary mask per tile + edge_policy on Image
 *        |
 *        v
 *   paint (paint_image_2d.cc) --> binary inside-test + blend weight for brush
 *        |
 *        v
 *   move/transform (move/transform.cc) --> extract fragment --> lift source
 *        |                                      |
 *        |                                      v
 *        +--> GPU preview (fragment preview buffers, two passes)
 *        |
 *        v
 *   commit --> write pixels + merge mask back to canvas
 * \endcode
 *
 * Consumed by paint_image_select_mask.cc, paint_image_select_move.cc,
 * paint_image_select_transform.cc, and paint_image_select_fragment.cc.
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

constexpr float SELECTION_MASK_THRESHOLD = IMAGE_PAINT_SELECTION_MASK_THRESHOLD;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared types
 * \{ */

struct SelectionTileFragmentPixelData {
  ImBuf *fragment_ibuf = nullptr;
  ImBuf *fragment_mask_ibuf = nullptr;
};

struct SelectionTileFragmentPreviewBuffers {
  ImBuf *fragment_blend_mask_ibuf = nullptr;
  ImBuf *fragment_display_ibuf = nullptr;
  ImBuf *fragment_feather_display_ibuf = nullptr;
};

struct SelectionTileFragmentGeometry {
  int2 origin_px = {0, 0};
  int2 size_px = {0, 0};
  int2 selection_origin_px = {0, 0};
  int2 selection_size_px = {0, 0};
  int2 tile_size_px = {1, 1};
  int tile_number = 1001;
};

struct SelectionTileFragment {
  SelectionTileFragmentPixelData pixels;
  SelectionTileFragmentPreviewBuffers preview;
  SelectionTileFragmentGeometry geom;
  PaintSelectionEdgePolicy edge_policy = BKE_image_paint_selection_edge_policy_feathered();
};

struct ImageSelectMoveState;
struct ImageSelectTransformState;

struct PaintSelectSession {
  ImageSelectMoveState *move = nullptr;
  ImageSelectTransformState *transform = nullptr;
};

/** \} */

} /* namespace blender */

#include "paint_image_select_fragment.hh"

namespace blender {

inline void selection_tile_fragment_free(SelectionTileFragment &frag)
{
  image_select_fragment_free(frag);
}

inline void selection_tile_fragments_free(Vector<SelectionTileFragment> &fragments)
{
  for (SelectionTileFragment &frag : fragments) {
    selection_tile_fragment_free(frag);
  }
  fragments.clear();
}

void paint_select_session_free(PaintSelectSession &session);

void image_select_move_commit(bContext *C, ImageSelectMoveState *state);
void image_select_move_state_free(ImageSelectMoveState *state);
void image_select_transform_state_free(ImageSelectTransformState *state);

bool image_paint_selection_poll(bContext *C);
int image_paint_selection_resolve_tile(Image *ima, const SpaceImage *sima, int preferred_tile);
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

inline int2 image_select_fragment_ui_origin(const SelectionTileFragment &frag)
{
  if (frag.geom.selection_size_px.x > 0 && frag.geom.selection_size_px.y > 0) {
    return frag.geom.selection_origin_px;
  }
  return frag.geom.origin_px;
}

inline int2 image_select_fragment_ui_size(const SelectionTileFragment &frag)
{
  if (frag.geom.selection_size_px.x > 0 && frag.geom.selection_size_px.y > 0) {
    return frag.geom.selection_size_px;
  }
  return frag.geom.size_px;
}

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
bool image_select_move_is_floating_in_space(const SpaceImage *sima);
wmOperatorStatus image_select_move_convert_to_transform(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event);

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

} /* namespace blender */
