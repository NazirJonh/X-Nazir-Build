/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_blender.hh"
#include "BKE_image.hh"
#include "BKE_library.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"
#include "ED_space_api.hh"
#include "ED_undo.hh"

#include "BKE_undo_system.hh"

#include "BIF_glutil.hh"

#include "GPU_immediate.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_geom.h"

#include "BLF_api.hh"
#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "../../space_image/image_runtime.hh"
#include "paint_image_select_intern.hh"


namespace blender {

/* -------------------------------------------------------------------- */
/** \name Internal helpers
 * \{ */

static void image_paint_selection_reset(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->image) {
    BKE_image_paint_selection_mask_free(sima->image);
  }

  Scene *scene = CTX_data_scene(C);
  scene->toolsettings->imapaint.use_selection_mask = 0;

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (sima && sima->image) {
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, sima->image);
  }
}


/**
 * Poll for all image paint selection operators.
 * Does not require an active brush -- selection tools are independent of the brush.
 * Blocked while a free-transform is in progress so that stray LMB clicks outside the
 * transform widget cannot accidentally start a new selection gesture.
 */
bool image_paint_selection_poll(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima) {
    return false;
  }
  if (sima->mode != SI_MODE_PAINT) {
    return false;
  }
  if (sima->image != nullptr &&
      (!ID_IS_EDITABLE(sima->image) || ID_IS_OVERRIDE_LIBRARY(sima->image)))
  {
    return false;
  }
  if (image_select_transform_is_floating(C)) {
    return false;
  }
  const ARegion *region = CTX_wm_region(C);
  if (!region || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  return true;
}

/** \} */
/* -------------------------------------------------------------------- */
/** \name Select All
 * \{ */

static wmOperatorStatus image_select_all_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Select All" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);

  /* Only select the active tile, not all tiles. */
  ImageUser iuser = sima->iuser;
  /* For UDIM images, sima->iuser.tile is reset to 0 after each temporary acquire.
   * The true active tile is tracked by ima->active_tile_index. */
  if (image->source == IMA_SRC_TILED) {
    const ImageTile *active = static_cast<const ImageTile *>(
        BLI_findlink(&image->tiles, image->active_tile_index));
    iuser.tile = active ? active->tile_number :
                          static_cast<const ImageTile *>(image->tiles.first)->tile_number;
  }
  const ImageTile *active_tile = BKE_image_get_tile_from_iuser(image, &iuser);
  if (!active_tile) {
    return OPERATOR_CANCELLED;
  }
  iuser.tile = active_tile->tile_number;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
  if (!ibuf) {
    return OPERATOR_CANCELLED;
  }

  ED_image_undo_push_begin_selection("Select All", image);

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  BKE_image_paint_selection_mask_get(image, active_tile->tile_number, ibuf->x, ibuf->y);
  BKE_image_paint_selection_mask_fill(image, active_tile->tile_number, 1.0f);

  BKE_image_release_ibuf(image, ibuf, nullptr);

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_all(wmOperatorType *ot)
{
  ot->name = "Select All";
  ot->idname = "PAINT_OT_image_select_all";
  ot->description = "Select the entire image as a paint mask";
  ot->exec = image_select_all_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select None
 * \{ */

static wmOperatorStatus image_select_none_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Select None" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Image *image = sima->image;

  if (image) {
    ED_image_undo_push_begin_selection("Select None", image);
  }

  if (image) {
    BKE_image_paint_selection_mask_free(image);
  }

  Scene *scene = CTX_data_scene(C);
  scene->toolsettings->imapaint.use_selection_mask = 0;

  DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (image) {
    ED_image_undo_push_end();
  }
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_none(wmOperatorType *ot)
{
  ot->name = "Select None";
  ot->idname = "PAINT_OT_image_select_none";
  ot->description = "Deselect the entire image (remove paint mask)";
  ot->exec = image_select_none_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Invert Selection
 * \{ */

static wmOperatorStatus image_select_invert_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;

  /* Commit any floating move-selection fragment before changing the selection state,
   * so the Move Selection undo step is closed before "Invert Selection" opens its own. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);
  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;

  ED_image_undo_push_begin_selection("Invert Selection", image);

  imapaint->use_selection_mask = 1;

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
    BKE_image_paint_selection_mask_invert(image, tile->tile_number);

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_invert(wmOperatorType *ot)
{
  ot->name = "Invert Selection";
  ot->idname = "PAINT_OT_image_select_invert";
  ot->description = "Invert the current paint selection mask";
  ot->exec = image_select_invert_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Box
 * \{ */

static wmOperatorStatus image_select_box_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  rctf rectf;
  WM_operator_properties_border_to_rctf(op, &rectf);

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  if (is_simple_click ||
      (BLI_rctf_size_x(&rectf) == 0.0f && BLI_rctf_size_y(&rectf) == 0.0f))
  {
    if (g_floating_state) {
      image_select_move_commit(C, g_floating_state);
      image_select_move_state_free(g_floating_state);
      g_floating_state = nullptr;
    }
    ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
    if (imapaint->use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  ui::view2d_region_to_view_rctf(&region->v2d, &rectf, &rectf);

  /* Auto-snap selection bounds to UDIM tile borders (integer UV coordinates) when close
   * enough. This makes it easy to select exactly one full tile or align to tile edges
   * without pixel-perfect cursor placement. Threshold: 2 % of one tile in UV units. */
  {
    const float snap_thresh = 0.02f;
    auto snap_to_udim_border = [](float v, float threshold) -> float {
      const float rounded = roundf(v);
      return (fabsf(v - rounded) < threshold) ? rounded : v;
    };
    rectf.xmin = snap_to_udim_border(rectf.xmin, snap_thresh);
    rectf.xmax = snap_to_udim_border(rectf.xmax, snap_thresh);
    rectf.ymin = snap_to_udim_border(rectf.ymin, snap_thresh);
    rectf.ymax = snap_to_udim_border(rectf.ymax, snap_thresh);
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));

  ED_image_undo_push_begin_selection("Box Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    /* UDIM tile UV origin. */
    const float uv_origin[2] = {float((tile->tile_number - 1001) % 10),
                                float((tile->tile_number - 1001) / 10)};

    /* Check if tile UV rect [origin, origin+1] intersects with box. */
    rctf tile_rect;
    tile_rect.xmin = uv_origin[0];
    tile_rect.xmax = uv_origin[0] + 1.0f;
    tile_rect.ymin = uv_origin[1];
    tile_rect.ymax = uv_origin[1] + 1.0f;

    if (BLI_rctf_isect(&tile_rect, &rectf, &tile_rect)) {
      ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
      float *data = mask->float_data_for_write();

      const int x1 = int(roundf((tile_rect.xmin - uv_origin[0]) * mask->x));
      const int y1 = int(roundf((tile_rect.ymin - uv_origin[1]) * mask->y));
      const int x2 = int(roundf((tile_rect.xmax - uv_origin[0]) * mask->x));
      const int y2 = int(roundf((tile_rect.ymax - uv_origin[1]) * mask->y));
      const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;

      for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
          if (x >= 0 && x < mask->x && y >= 0 && y < mask->y) {
            data[y * mask->x + x] = fill_value;
          }
        }
      }
    }

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  ED_region_tag_redraw(region);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

static wmOperatorStatus image_select_box_invoke(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  if (image_select_move_delegate_to_move_operator(C, event)) {
    return OPERATOR_FINISHED;
  }
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return WM_gesture_box_invoke(C, op, event);
}

static wmOperatorStatus image_select_box_modal(bContext *C,
                                               wmOperator *op,
                                               const wmEvent *event)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (event->type == MOUSEMOVE && gesture) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > 3 || abs(event->xy[1] - start_y) > 3) {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
  return WM_gesture_box_modal(C, op, event);
}

void PAINT_OT_image_select_box(wmOperatorType *ot)
{
  ot->name = "Select Box";
  ot->idname = "PAINT_OT_image_select_box";
  ot->description = "Select a rectangular region as a paint mask";

  ot->invoke = image_select_box_invoke;
  ot->modal = image_select_box_modal;
  ot->exec = image_select_box_exec;
  ot->cancel = WM_gesture_box_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_box(ot);
  WM_operator_properties_select_operation_simple(ot);

  RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Lasso
 * \{ */

/**
 * Scanline polygon fill for a 1-channel float buffer.
 * Fills the interior of \a points with \a color.
 * Requires at least 3 points; silently does nothing otherwise.
 */
static void fill_polygon_float(ImBuf *ibuf, Span<int2> points, float color)
{
  if (points.size() < 3) {
    return;
  }

  const int width = ibuf->x;
  const int height = ibuf->y;

  int min_y = points[0].y;
  int max_y = points[0].y;
  for (const int2 &p : points) {
    min_y = min_ii(min_y, p.y);
    max_y = max_ii(max_y, p.y);
  }
  min_y = max_ii(min_y, 0);
  max_y = min_ii(max_y, height - 1);

  Vector<int> intersections;
  for (int y = min_y; y <= max_y; y++) {
    intersections.clear();
    const int n = points.size();
    for (int i = 0; i < n; i++) {
      const int next = (i + 1) % n;
      const int y1 = points[i].y;
      const int y2 = points[next].y;
      if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
        const float x = float(y - y1) * float(points[next].x - points[i].x) /
                            float(y2 - y1) +
                        float(points[i].x);
        intersections.append(int(x));
      }
    }
    std::sort(intersections.begin(), intersections.end());

    for (int i = 0; i + 1 < int(intersections.size()); i += 2) {
      const int x1 = max_ii(intersections[i], 0);
      const int x2 = min_ii(intersections[i + 1], width - 1);
      float *row = ibuf->float_data_for_write() + y * width;
      for (int x = x1; x <= x2; x++) {
        row[x] = color;
      }
    }
  }
}

static wmOperatorStatus image_select_lasso_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  if (image_select_move_delegate_to_move_operator(C, event)) {
    return OPERATOR_FINISHED;
  }
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return WM_gesture_lasso_invoke(C, op, event);
}

static wmOperatorStatus image_select_lasso_modal(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (event->type == MOUSEMOVE && gesture) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > 3 || abs(event->xy[1] - start_y) > 3) {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
  return WM_gesture_lasso_modal(C, op, event);
}

static wmOperatorStatus image_select_lasso_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  if (is_simple_click) {
    if (g_floating_state) {
      image_select_move_commit(C, g_floating_state);
      image_select_move_state_free(g_floating_state);
      g_floating_state = nullptr;
    }
    if (scene->toolsettings->imapaint.use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  Array<int2> mcoords = WM_gesture_lasso_path_to_array(C, op);
  if (mcoords.is_empty() || int(mcoords.size()) < 3) {
    if (scene->toolsettings->imapaint.use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));

  ED_image_undo_push_begin_selection("Lasso Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  /* Convert lasso points to UV space once. */
  Vector<float2> uv_points;
  uv_points.reserve(mcoords.size());
  rctf lasso_uv_bounds;
  BLI_rctf_init_minmax(&lasso_uv_bounds);
  for (const int2 &p : mcoords) {
    float co[2] = {float(p.x), float(p.y)};
    ui::view2d_region_to_view(&region->v2d, co[0], co[1], &co[0], &co[1]);
    uv_points.append(float2(co[0], co[1]));
    BLI_rctf_do_minmax_v(&lasso_uv_bounds, co);
  }

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    /* UDIM tile UV origin. */
    const float uv_origin[2] = {float((tile->tile_number - 1001) % 10),
                                float((tile->tile_number - 1001) / 10)};

    rctf tile_rect;
    tile_rect.xmin = uv_origin[0];
    tile_rect.xmax = uv_origin[0] + 1.0f;
    tile_rect.ymin = uv_origin[1];
    tile_rect.ymax = uv_origin[1] + 1.0f;
    if (!BLI_rctf_isect(&tile_rect, &lasso_uv_bounds, nullptr)) {
      BKE_image_release_ibuf(image, ibuf, nullptr);
      continue;
    }

    ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);

    /* Convert UV points to tile pixel space. */
    Vector<int2> tile_points;
    tile_points.reserve(uv_points.size());
    for (const float2 &uv : uv_points) {
      tile_points.append(
          int2(int(roundf((uv.x - uv_origin[0]) * mask->x)), int(roundf((uv.y - uv_origin[1]) * mask->y))));
    }

    const float color = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
    fill_polygon_float(mask, tile_points, color);

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  ED_region_tag_redraw(region);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_lasso(wmOperatorType *ot)
{
  ot->name = "Select Lasso";
  ot->idname = "PAINT_OT_image_select_lasso";
  ot->description = "Select a freehand region as a paint mask";

  ot->invoke = image_select_lasso_invoke;
  ot->modal = image_select_lasso_modal;
  ot->exec = image_select_lasso_exec;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_lasso(ot);
  WM_operator_properties_select_operation_simple(ot);

  RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Select Circle
 * \{ */

/**
 * Rasterize a filled ellipse into a 1-channel float buffer.
 * Passing equal #rx and #ry produces a perfect circle.
 * Separate radii are needed because UV space is not always isotropic --
 * the Image Editor view2d can have a non-1:1 aspect ratio (e.g. when
 * multiple UDIM tiles are visible side-by-side), so the screen-pixel
 * radius maps to different UV extents along X and Y.
 */
static void fill_circle_float(ImBuf *ibuf, int cx, int cy, int rx, int ry, float color)
{
  const int width = ibuf->x;
  const int height = ibuf->y;
  const int x1 = max_ii(cx - rx, 0);
  const int x2 = min_ii(cx + rx, width - 1);
  const int y1 = max_ii(cy - ry, 0);
  const int y2 = min_ii(cy + ry, height - 1);
  const float rx2 = float(rx) * float(rx);
  const float ry2 = float(ry) * float(ry);

  for (int y = y1; y <= y2; y++) {
    const float dy = float(y - cy);
    const float dy2 = (dy * dy) / ry2;
    for (int x = x1; x <= x2; x++) {
      const float dx = float(x - cx);
      if ((dx * dx) / rx2 + dy2 <= 1.0f) {
        ibuf->float_data_for_write()[y * width + x] = color;
      }
    }
  }
}

static wmOperatorStatus image_select_circle_invoke(bContext *C,
                                                    wmOperator *op,
                                                    const wmEvent *event)
{
  if (image_select_move_delegate_to_move_operator(C, event)) {
    return OPERATOR_FINISHED;
  }
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return WM_gesture_circle_invoke(C, op, event);
}

static wmOperatorStatus image_select_circle_modal(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent *event)
{
  wmGesture *gesture = static_cast<wmGesture *>(op->customdata);
  if (event->type == MOUSEMOVE && gesture) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > 3 || abs(event->xy[1] - start_y) > 3) {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
  return WM_gesture_circle_modal(C, op, event);
}

static wmOperatorStatus image_select_circle_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!sima || !sima->runtime) {
    return OPERATOR_CANCELLED;
  }
  ImageSelectMoveState *&g_floating_state = sima->runtime->paint_select.move;
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  const bool is_simple_click = RNA_boolean_get(op->ptr, "is_simple_click");
  const int mradius = RNA_int_get(op->ptr, "radius");

  if (is_simple_click || mradius <= 0) {
    if (g_floating_state) {
      image_select_move_commit(C, g_floating_state);
      image_select_move_state_free(g_floating_state);
      g_floating_state = nullptr;
    }
    if (scene->toolsettings->imapaint.use_selection_mask) {
      ED_image_undo_push_begin_selection("Deselect", image);
      image_paint_selection_reset(C);
      ED_image_undo_push_end();
    }
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  if (g_floating_state) {
    image_select_move_commit(C, g_floating_state);
    image_select_move_state_free(g_floating_state);
    g_floating_state = nullptr;
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));

  ED_image_undo_push_begin_selection("Circle Select", image);

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  ImagePaintSettings *imapaint = &scene->toolsettings->imapaint;
  imapaint->use_selection_mask = 1;

  const int mx = RNA_int_get(op->ptr, "x");
  const int my = RNA_int_get(op->ptr, "y");

  /* Convert center to UV space. */
  float co_center[2] = {float(mx), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_center[0], co_center[1], &co_center[0], &co_center[1]);

  /* Convert radius to UV space along X: measure a point one radius to the right. */
  float co_edge[2] = {float(mx + mradius), float(my)};
  ui::view2d_region_to_view(&region->v2d, co_edge[0], co_edge[1], &co_edge[0], &co_edge[1]);
  const float uv_radius = co_edge[0] - co_center[0];

  /* Convert radius to UV space along Y separately.
   * The Image Editor view2d can have a non-1:1 pixel-to-UV ratio (e.g. when multiple
   * UDIM tiles are shown side-by-side), so the horizontal and vertical UV extents of
   * the same screen-pixel radius differ. Without this correction the circle appears as
   * a flattened ellipse on all UDIM tiles except the first one. */
  float co_edge_y[2] = {float(mx), float(my + mradius)};
  ui::view2d_region_to_view(&region->v2d, co_edge_y[0], co_edge_y[1], &co_edge_y[0], &co_edge_y[1]);
  const float uv_radius_y = fabsf(co_edge_y[1] - co_center[1]);

  const float abs_uv_radius = fabsf(uv_radius);
  rctf circle_uv_bounds;
  circle_uv_bounds.xmin = co_center[0] - abs_uv_radius;
  circle_uv_bounds.xmax = co_center[0] + abs_uv_radius;
  circle_uv_bounds.ymin = co_center[1] - uv_radius_y;
  circle_uv_bounds.ymax = co_center[1] + uv_radius_y;

  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    /* UDIM tile UV origin. */
    const float uv_origin[2] = {float((tile->tile_number - 1001) % 10),
                                float((tile->tile_number - 1001) / 10)};

    rctf tile_rect;
    tile_rect.xmin = uv_origin[0];
    tile_rect.xmax = uv_origin[0] + 1.0f;
    tile_rect.ymin = uv_origin[1];
    tile_rect.ymax = uv_origin[1] + 1.0f;
    if (!BLI_rctf_isect(&tile_rect, &circle_uv_bounds, nullptr)) {
      BKE_image_release_ibuf(image, ibuf, nullptr);
      continue;
    }

    ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);

    const int cx = int(roundf((co_center[0] - uv_origin[0]) * mask->x));
    const int cy = int(roundf((co_center[1] - uv_origin[1]) * mask->y));
    int rx = int(roundf(uv_radius * mask->x));
    if (rx <= 0) {
      rx = 1;
    }
    /* Compute Y pixel radius from the separately measured UV Y radius so the circle is
     * correct on non-square tiles and in views with a non-1:1 aspect ratio. */
    int ry = int(roundf(uv_radius_y * mask->y));
    if (ry <= 0) {
      ry = 1;
    }

    const float color = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
    fill_circle_float(mask, cx, cy, rx, ry, color);

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

void PAINT_OT_image_select_circle(wmOperatorType *ot)
{
  ot->name = "Select Circle";
  ot->idname = "PAINT_OT_image_select_circle";
  ot->description = "Select a circular region as a paint mask";

  ot->invoke = image_select_circle_invoke;
  ot->modal = image_select_circle_modal;
  ot->exec = image_select_circle_exec;
  ot->cancel = WM_gesture_circle_cancel;
  ot->poll = image_paint_selection_poll;
  ot->flag = OPTYPE_REGISTER;

  WM_operator_properties_gesture_circle(ot);
  WM_operator_properties_select_operation_simple(ot);

  RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lifetime
 * \{ */

void paint_select_session_free(PaintSelectSession &session)
{
  if (session.move) {
    image_select_move_state_free(session.move);
    session.move = nullptr;
  }
  if (session.transform) {
    image_select_transform_state_free(session.transform);
    session.transform = nullptr;
  }
}

/** \} */

}  /* namespace blender */
