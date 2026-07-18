/* SPDX-FileCopyrightText: 2026 Blender Authors
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

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"

#include "IMB_imbuf.hh"

/* #PaintSelectSession is stored by value on #SpaceImage_Runtime. The dependency only goes this
 * way: space_image never includes anything from this module. */
#include "../../space_image/image_runtime.hh"

/* Base state and lifetime helpers shared by move / transform / warp. Included before the
 * per-tool headers below, which derive their state structs from it. */
#include "paint_image_select_floating.hh"

/* Owns #SelectionTileFragment and the lift/restore/preview API operating on it. The type must be
 * complete here because the per-tool headers below store fragments by value and in #Vector. */
#include "paint_image_select_fragment.hh"

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

/**
 * Restore the tile backups of \a sima's floating gradient session and free it, if it has one.
 * No-op otherwise. Called through #image_select_floating_sessions_end; see that function for why
 * the gradient is discarded where the lifted-fragment tools are committed.
 */
void image_select_gradient_session_end_for_takeover(bContext *C, SpaceImage *sima);

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
