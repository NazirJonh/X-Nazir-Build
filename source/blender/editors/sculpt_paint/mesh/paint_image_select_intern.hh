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
struct ImageSelectGradientState;
struct ImageSelectWarpState;

struct PaintSelectSession {
  ImageSelectMoveState *move = nullptr;
  ImageSelectTransformState *transform = nullptr;
  ImageSelectGradientState *gradient = nullptr;
  ImageSelectWarpState *warp = nullptr;
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

bool image_paint_selection_poll(bContext *C);
int image_paint_selection_resolve_tile(Image *ima, const SpaceImage *sima, int preferred_tile);

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

/** Column/row (0-based) of a 1001-based UDIM tile number in the 10-wide tile grid. */
inline int2 image_select_udim_tile_col_row(int tile_number)
{
  return int2((tile_number - 1001) % 10, (tile_number - 1001) / 10);
}

/** Bottom-left UV corner of a UDIM tile (its column/row in whole-tile UV units). */
inline float2 image_select_udim_tile_uv_origin(int tile_number)
{
  const int2 col_row = image_select_udim_tile_col_row(tile_number);
  return float2(float(col_row.x), float(col_row.y));
}

/* Gradient floating-state helpers. The gradient rasterization API lives in
 * paint_image_select_gradient.hh; these operate on the runtime floating session. */
bool image_select_gradient_is_floating(bContext *C);
bool image_select_gradient_is_floating_in_space(const SpaceImage *sima);
void image_select_gradient_state_free(ImageSelectGradientState *state);

/* Mask selection operators (box/lasso/circle and friends). */
void PAINT_OT_image_select_all(wmOperatorType *ot);
void PAINT_OT_image_select_none(wmOperatorType *ot);
void PAINT_OT_image_select_invert(wmOperatorType *ot);
void PAINT_OT_image_select_box(wmOperatorType *ot);
void PAINT_OT_image_select_lasso(wmOperatorType *ot);
void PAINT_OT_image_select_circle(wmOperatorType *ot);

/* Gradient tool operators. */
void PAINT_OT_image_select_gradient(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_apply(wmOperatorType *ot);
void PAINT_OT_image_select_gradient_cancel(wmOperatorType *ot);

} /* namespace blender */

/* Subsystem declarations. Included here (after the shared types above) so the existing consumers
 * keep a single include; the move/transform/warp declarations physically live in their own
 * files. */
#include "paint_image_select_move_intern.hh"
#include "paint_image_select_transform_intern.hh"
#include "paint_image_select_warp_intern.hh"
