/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The one implementation of the box / lasso / circle selection sequence.
 * See paint_image_select_gesture.hh for the rationale and the shape contract.
 */

#include <climits>
#include <cstdlib>

#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_utildefines.h"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"

#include "DEG_depsgraph.hh"

#include "IMB_imbuf_types.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_select_utils.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../../space_image/image_runtime.hh"
#include "paint_image_select_gesture.hh"
#include "paint_image_select_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Simple-click bookkeeping
 * \{ */

/**
 * Cursor travel (in pixels) above which a press-release is treated as a drag, not a simple click.
 */
constexpr int IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX = 3;

bool image_select_gesture_invoke_begin(bContext *C, wmOperator *op, const wmEvent *event)
{
  if (image_select_move_delegate_to_move_operator(C, event)) {
    return true;
  }
  RNA_int_set(op->ptr, "click_x", event->xy[0]);
  RNA_int_set(op->ptr, "click_y", event->xy[1]);
  RNA_boolean_set(op->ptr, "is_simple_click", true);
  return false;
}

void image_select_gesture_drag_detect(wmOperator *op, const wmEvent *event)
{
  if (event->type == MOUSEMOVE && op->customdata) {
    const int start_x = RNA_int_get(op->ptr, "click_x");
    const int start_y = RNA_int_get(op->ptr, "click_y");
    if (abs(event->xy[0] - start_x) > IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX ||
        abs(event->xy[1] - start_y) > IMAGE_SELECT_CLICK_DRAG_THRESHOLD_PX)
    {
      RNA_boolean_set(op->ptr, "is_simple_click", false);
    }
  }
}

void image_select_gesture_properties(wmOperatorType *ot)
{
  /* These three carry per-invocation cursor state between invoke/modal and exec; they are not user
   * settings. Without PROP_SKIP_SAVE a stored `is_simple_click = true` would be replayed by
   * `operator_repeat`, turning a repeated selection into a deselect. PROP_HIDDEN additionally
   * keeps them out of the "Adjust Last Operation" panel. Same treatment as the transient `x` /
   * `y` / `wait_for_input` properties in #WM_operator_properties_gesture_circle. */
  PropertyRNA *prop;
  prop = RNA_def_boolean(ot->srna, "is_simple_click", false, "Simple Click", "Click without drag");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_int(ot->srna, "click_x", 0, INT_MIN, INT_MAX, "Click X", "", INT_MIN, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  prop = RNA_def_int(ot->srna, "click_y", 0, INT_MIN, INT_MAX, "Click Y", "", INT_MIN, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared sequence steps
 * \{ */

static void image_paint_selection_reset(bContext *C)
{
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima && sima->image) {
    BKE_image_paint_selection_mask_free(sima->image);
  }

  if (Scene *scene = CTX_data_scene(C)) {
    DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
    DEG_id_tag_update(&scene->id, ID_RECALC_SYNC_TO_EVAL);
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  if (sima && sima->image) {
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, sima->image);
  }
}

/**
 * End every floating session of this editor before the gesture opens its own undo step.
 *
 * Not just the move session: transform and warp hold an open image undo step too, and
 * #ED_image_undo_push_begin_selection below would free it from under them. See
 * #image_select_floating_sessions_end.
 */
static void image_select_commit_floating_ops(bContext *C, SpaceImage *sima)
{
  image_select_floating_sessions_end(C,
                                     sima,
                                     IMAGE_SELECT_FLOATING_TOOL_MOVE |
                                         IMAGE_SELECT_FLOATING_TOOL_TRANSFORM |
                                         IMAGE_SELECT_FLOATING_TOOL_GRADIENT |
                                         IMAGE_SELECT_FLOATING_TOOL_WARP);
}

/** Clear the whole selection (used for simple-click and empty-gesture paths). */
static void image_select_apply_deselect(bContext *C, Image *image)
{
  if (BKE_image_paint_selection_mask_has_any(image)) {
    ED_image_undo_push_begin_selection("Deselect", image);
    image_paint_selection_reset(C);
    ED_image_undo_push_end();
  }
}

/**
 * Pixels are rasterized unconditionally unless island expansion is enabled, in which case only
 * ADD and SUB still need the raw gesture footprint on top of the expanded islands.
 */
static bool image_paint_selection_should_rasterize_gesture(const Scene *scene,
                                                           const eSelectOp sel_op)
{
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  if (!imapaint.use_selection_uv_island) {
    return true;
  }
  return ELEM(sel_op, SEL_OP_ADD, SEL_OP_SUB);
}

static bool image_paint_selection_tile_intersection_get(const float2 &uv_origin,
                                                        const rctf &gesture_uv_bounds,
                                                        rctf *r_intersection)
{
  rctf tile_rect;
  tile_rect.xmin = uv_origin.x;
  tile_rect.xmax = uv_origin.x + 1.0f;
  tile_rect.ymin = uv_origin.y;
  tile_rect.ymax = uv_origin.y + 1.0f;
  return BLI_rctf_isect(&tile_rect, &gesture_uv_bounds, r_intersection);
}

/** Rasterize \a shape into every UDIM tile whose UV square overlaps \a gesture_uv_bounds. */
static void image_select_gesture_rasterize_tiles(SpaceImage *sima,
                                                 Image *image,
                                                 const rctf &gesture_uv_bounds,
                                                 const ImageSelectGestureShape &shape,
                                                 const float fill_value)
{
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = sima->iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    const float2 uv_origin = image_select_udim_tile_uv_origin(tile->tile_number);
    rctf tile_uv_rect;
    if (image_paint_selection_tile_intersection_get(uv_origin, gesture_uv_bounds, &tile_uv_rect)) {
      ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
      shape.rasterize_tile(uv_origin, tile_uv_rect, mask, fill_value);
    }

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic gesture exec
 * \{ */

wmOperatorStatus image_select_gesture_exec_generic(bContext *C,
                                                   wmOperator *op,
                                                   ImageSelectGestureShape &shape)
{
  Scene *scene = CTX_data_scene(C);
  ARegion *region = CTX_wm_region(C);
  SpaceImage *sima = CTX_wm_space_image(C);
  if (!scene || !sima || !sima->runtime || !region) {
    return OPERATOR_CANCELLED;
  }
  Image *image = sima->image;
  if (!image) {
    return OPERATOR_CANCELLED;
  }

  /* A press-release without travel means "clear the selection", never "select nothing". */
  if (RNA_boolean_get(op->ptr, "is_simple_click")) {
    image_select_commit_floating_ops(C, sima);
    image_select_apply_deselect(C, image);
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  image_select_commit_floating_ops(C, sima);

  rctf gesture_uv_bounds;
  if (!shape.uv_bounds_calc(region, op, gesture_uv_bounds)) {
    /* Degenerate gesture: behaves like a simple click. */
    image_select_apply_deselect(C, image);
    return OPERATOR_FINISHED;
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  const bool rasterize_gesture = image_paint_selection_should_rasterize_gesture(scene, sel_op);
  const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;

  ED_image_undo_push_begin_selection(shape.undo_name(), image);

  /* Subtract: expand islands before filling pixels so we seed from the gesture geometry,
   * not from remaining selected pixels (which would wrongly deselect unrelated islands). */
  if (sel_op == SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &gesture_uv_bounds);
  }

  if (sel_op == SEL_OP_SET) {
    BKE_image_paint_selection_mask_free(image);
  }

  if (rasterize_gesture) {
    image_select_gesture_rasterize_tiles(sima, image, gesture_uv_bounds, shape, fill_value);
  }

  /* Add/set: expand to full UV islands touched by the gesture. */
  if (sel_op != SEL_OP_SUB) {
    image_paint_selection_expand_uv_islands(C, image, sel_op, &gesture_uv_bounds);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_EDITORS);
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  ED_region_tag_redraw(region);

  BKE_image_paint_selection_edge_policy_set(image, shape.edge_policy());
  BKE_image_paint_selection_blend_mask_invalidate(image);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

/** \} */

}  // namespace blender
