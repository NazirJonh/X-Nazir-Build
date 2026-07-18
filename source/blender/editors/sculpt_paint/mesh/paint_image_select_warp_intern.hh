/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Declarations for the mesh-warp part of the Image Paint selection system.
 *
 * NOTE: This header is included at the end of #paint_image_select_intern.hh, which provides the
 * shared types it relies on (#SelectionTileFragment, etc.). Include
 * `paint_image_select_intern.hh` rather than this file directly.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

struct wmPaintCursor;

namespace blender {

struct ReportList;

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

constexpr int IMAGE_SELECT_WARP_GRID_SIZE = 4;
constexpr int IMAGE_SELECT_WARP_POINT_COUNT = IMAGE_SELECT_WARP_GRID_SIZE *
                                              IMAGE_SELECT_WARP_GRID_SIZE;
/* Precedent: brush AA padding uses 4px in paint_image_2d.cc:773. No existing named margin
 * constant in mesh/ fits this use case, so this is a new one. */
constexpr int IMAGE_SELECT_WARP_MIN_MARGIN_PX = 4;

/* Bounds for #ImagePaintSettings::warp_grid_size (also enforced by its RNA property range). */
constexpr int IMAGE_SELECT_WARP_GRID_MIN = 2;
constexpr int IMAGE_SELECT_WARP_GRID_MAX = 10;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Floating state queries
 * \{ */

bool image_select_warp_is_floating(bContext *C);
bool image_select_warp_is_floating_in_space(const SpaceImage *sima);

void PAINT_OT_image_select_warp(wmOperatorType *ot);
void PAINT_OT_image_select_warp_confirm(wmOperatorType *ot);
void PAINT_OT_image_select_warp_cancel(wmOperatorType *ot);
void PAINT_OT_image_select_warp_undo_step(wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bilinear cell math (pure, unit-tested in paint_image_select_warp_tests.cc)
 * \{ */

/** Evaluate a bilinear (Coons) patch at local coordinates (u, v) in [0, 1] x [0, 1]. */
float2 warp_bilinear_eval(const float2 &p00,
                          const float2 &p10,
                          const float2 &p01,
                          const float2 &p11,
                          float u,
                          float v);

/**
 * Evaluate the grid_size^2 control grid at continuous grid coordinates (gx, gy) in
 * [0, grid_size-1] x [0, grid_size-1] with a tensor-product Catmull-Rom spline. Interpolating
 * (returns control points exactly at integer coords) and C1-continuous across cell seams.
 * Border phantom control points are linearly extrapolated (p[-1] = 2*p[0] - p[1]) so the spline
 * reproduces affine fields exactly at the edges too, keeping an evenly spaced grid evenly spaced.
 */
float2 warp_grid_eval_smooth(const float2 *pts, int grid_size, float gx, float gy);

/** Fetch the 4 corners of cell (cell_x, cell_y) from a grid_size^2 point grid. */
inline void warp_grid_cell_corners(const float2 *pts,
                                   int grid_size,
                                   int cell_x,
                                   int cell_y,
                                   float2 *r_p00,
                                   float2 *r_p10,
                                   float2 *r_p01,
                                   float2 *r_p11)
{
  const int i00 = cell_y * grid_size + cell_x;
  *r_p00 = pts[i00];
  *r_p10 = pts[i00 + 1];
  *r_p01 = pts[i00 + grid_size];
  *r_p11 = pts[i00 + grid_size + 1];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name State
 * \{ */

struct ImageSelectWarpState : public PaintSelectFloatingSession {
  static constexpr PaintSelectTool tool_type = PaintSelectTool::Warp;
  ImageSelectWarpState() : PaintSelectFloatingSession(tool_type) {}

  int tile_number = 1001;

  /* Single-tile capture: geom.origin_px/size_px is the margin-expanded capture area;
   * geom.selection_origin_px/selection_size_px is the tight selection bbox, both in
   * fragment-local (capture-relative) pixel coordinates -- see paint_image_select_warp.cc:
   * image_select_warp_extract(). */
  SelectionTileFragment fragment;

  /* Number of control points along each grid side (from ImagePaintSettings::warp_grid_size,
   * clamped to [IMAGE_SELECT_WARP_GRID_MIN, IMAGE_SELECT_WARP_GRID_MAX]). Live-updated while
   * floating -- see #applied_settings_revision. */
  int grid_size = IMAGE_SELECT_WARP_GRID_SIZE;
  /* Value of the global warp-settings revision counter (bumped by the "warp_grid_size" RNA
   * property's update callback) last applied to this session. Compared against the live counter
   * in the paint-cursor hover detector, which resizes the grid when they differ -- see
   * image_select_warp_resize_grid() in paint_image_select_warp.cc. */
  uint64_t applied_settings_revision = 0;
  /* Interpolation mode for grid evaluation (from ImagePaintSettings::warp_interpolation). Read at
   * invoke and live-updated alongside grid_size -- see
   * image_select_warp_apply_settings_revision. */
  eImagePaint_WarpInterpolation interp = IMAGE_PAINT_WARP_INTERP_LINEAR;

  /* Grid point coordinates normalized to the capture area: [0,0] = capture top-left corner,
   * [1,1] = capture bottom-right corner. Row-major, grid_size x grid_size (grid_size * grid_size
   * elements). */
  Array<float2> src_pts;
  Array<float2> tgt_pts;

  /* Paint-cursor "detector": runs on mouse move (and on most redraws) without a running modal
   * operator, so it updates #hover_point_idx and requests a redraw without blocking View2D
   * pan/zoom or UI interaction the way an always-running modal handler would (a running modal
   * operator forces #WM_HANDLER_BREAK for every event it doesn't explicitly pass on to a
   * keymap-bound operator, which stops area/region event dispatch -- including navigation --
   * entirely; see `wm_handler_operator_call()` in wm_event_system.cc). The actual grid/handle
   * drawing stays in #draw_handle's REGION_DRAW_POST_PIXEL callback. */
  wmPaintCursor *paint_cursor = nullptr;

  int drag_point_idx = -1;
  /* Index of the control point under the cursor while not dragging (-1 = none), for hover
   * highlighting. Updated by #paint_cursor's callback. */
  int hover_point_idx = -1;
  /* Snapshot of all tgt_pts taken before each drag gesture (LMB press), for
   * PAINT_OT_image_select_warp_undo_step (Ctrl+Z while floating). */
  Vector<Array<float2>> drag_position_history;
};

void image_select_warp_state_free(ImageSelectWarpState *state);

/**
 * \a sima's live floating session when it is a warp session, else null.
 *
 * The typed accessor for callers outside paint_image_select_warp.cc; see
 * #image_select_move_state_get for why the template accessor is not used across files.
 */
ImageSelectWarpState *image_select_warp_state_get(SpaceImage *sima);

/** Tag-dispatched entry point of #image_select_floating_session_free. */
void image_select_warp_session_free(PaintSelectFloatingSession *session);

/**
 * Commit and free \a sima's floating warp session, if it has one. No-op otherwise.
 * Called through #image_select_floating_sessions_end; see that function for the semantics.
 */
void image_select_warp_session_end_for_takeover(bContext *C, SpaceImage *sima);

/**
 * Capture a single-tile fragment for warp: the tight selection bbox expanded by a margin
 * (max(half the tight bbox size, IMAGE_SELECT_WARP_MIN_MARGIN_PX) per side, clamped to the tile),
 * so the warp grid has real canvas content to sample when points are dragged outward.
 * `r_fragment->geom.origin_px/size_px` = the expanded capture area (what gets lifted from the
 * canvas); `r_fragment->geom.selection_origin_px/selection_size_px` = the tight selection bbox,
 * in fragment-local (capture-relative) pixel coordinates.
 */
bool image_select_warp_extract(wmOperator *op,
                               Image *ima,
                               const ImageUser &base_iuser,
                               int tile_number,
                               SelectionTileFragment *r_fragment);

/** Place src_pts == tgt_pts on an evenly spaced grid covering the tight selection bbox
 * (state->fragment.geom.selection_*), normalized to the capture area
 * (state->fragment.geom.size_px). */
void image_select_warp_init_grid(ImageSelectWarpState *state);

/** Rasterize the current tgt_pts deformation into the canvas at the fragment's capture area,
 * update the destination selection mask, and close the undo step opened at lift time. */
void image_select_warp_commit(bContext *C, ImageSelectWarpState *state, ReportList *reports);

/** \} */

} /* namespace blender */
