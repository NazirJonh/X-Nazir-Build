/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The one implementation of the box / lasso / circle selection sequence.
 * See paint_image_select_gesture.hh for the rationale and the shape contract.
 */

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

#include "BLI_bitmap_draw_2d.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "IMB_imbuf_types.hh"

#include "ED_image.hh"
#include "ED_image_paint_symmetry.hh"
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
/* #image_select_move_delegate_to_move_operator only. */
#include "paint_image_select_move_intern.hh"

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

static void image_paint_selection_reset(bContext *C,
                                        const Span<ImagePaintSelectionTarget> targets)
{
  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_mask_free(target.image);
  }
  image_paint_selection_targets_update(C, targets);
}

/**
 * End the floating session of this editor before the gesture opens its own undo step.
 *
 * Whichever tool it belongs to: a lifted session holds an open image undo step, and
 * #image_paint_selection_undo_begin below would free it from under it. See
 * #image_select_floating_sessions_end_all.
 */
static void image_select_commit_floating_ops(bContext *C, SpaceImage *sima)
{
  image_select_floating_sessions_end_all(C, sima);
}

/** Clear the whole selection (used for simple-click and empty-gesture paths). */
static void image_select_apply_deselect(bContext *C, const Span<ImagePaintSelectionTarget> targets)
{
  if (std::any_of(targets.begin(), targets.end(), [](const ImagePaintSelectionTarget &target) {
        return BKE_image_paint_selection_mask_has_any(target.image);
      }))
  {
    image_paint_selection_undo_begin("Deselect", targets);
    image_paint_selection_reset(C, targets);
    ED_image_undo_push_end();
  }
}

/**
 * Pixels are rasterized unconditionally in Pixels mode. Face mode never rasterizes the raw
 * gesture (only complete faces). Island mode still rasterizes the gesture for ADD and SUB so the
 * raw footprint sits on top of the expanded islands.
 */
static bool image_paint_selection_should_rasterize_gesture(const Scene *scene,
                                                           const eSelectOp sel_op)
{
  const ImagePaintSettings &imapaint = scene->toolsettings->imapaint;
  switch (eImagePaint_SelectionExpand(imapaint.selection_expand)) {
    case IMAGE_PAINT_SELECT_EXPAND_FACE:
      return false;
    case IMAGE_PAINT_SELECT_EXPAND_ISLAND:
      return ELEM(sel_op, SEL_OP_ADD, SEL_OP_SUB);
    case IMAGE_PAINT_SELECT_EXPAND_PIXELS:
    case IMAGE_PAINT_SELECT_EXPAND_MESH:
      return true;
  }
  return true;
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

/**
 * Image-editor-space symmetry for selection gestures: which tile centerlines to mirror across,
 * from the image paint symmetry flags. The flags are shared with brush painting, so enabling X
 * symmetry in the viewport makes a gesture mask the mirrored side of every tile as well.
 *
 * The mirror deliberately stays in image editor (UV) space: the mirrored location of a brush dab
 * is ultimately mesh-dependent, but resolving that per mask pixel would need the full surface
 * lookup, and a symmetric unwrap places the mirrored faces in the mirrored tile region anyway.
 */
static void image_select_gesture_mirror_flags_get(const Scene *scene, bool &r_mirror_x, bool &r_mirror_y)
{
  const ePaintSymmetryFlags symmetry = scene->toolsettings->imapaint.paint.symmetry_flags;
  r_mirror_x = bool(symmetry & PAINT_SYMM_X);
  r_mirror_y = bool(symmetry & PAINT_SYMM_Y);
}

/**
 * UV bounds of the gesture mirrored across the centerlines of every tile it touches.
 *
 * The mirror is per tile, so the mirrored bounds are the union of the mirrored intersections
 * with each tile rather than one global flip of \a gesture_uv_bounds. Face/island expansion uses
 * these bounds exactly like the original ones; the pixel-accurate mirrored mask comes from
 * #ImageSelectGestureShape::rasterize_tile_mirrored.
 */
static rctf image_select_gesture_mirrored_bounds(const rctf &gesture_uv_bounds,
                                                 const bool mirror_x,
                                                 const bool mirror_y)
{
  rctf mirrored;
  BLI_rctf_init_minmax(&mirrored);
  const int min_col = int(floorf(gesture_uv_bounds.xmin));
  const int max_col = int(floorf(gesture_uv_bounds.xmax));
  const int min_row = int(floorf(gesture_uv_bounds.ymin));
  const int max_row = int(floorf(gesture_uv_bounds.ymax));
  for (int col = min_col; col <= max_col; col++) {
    for (int row = min_row; row <= max_row; row++) {
      rctf tile_rect;
      tile_rect.xmin = float(col);
      tile_rect.xmax = float(col + 1);
      tile_rect.ymin = float(row);
      tile_rect.ymax = float(row + 1);
      rctf isect;
      if (!BLI_rctf_isect(&tile_rect, &gesture_uv_bounds, &isect)) {
        continue;
      }
      if (mirror_x) {
        const float flipped_xmin = float(col + 1) - isect.xmax;
        const float flipped_xmax = float(col + 1) - isect.xmin;
        isect.xmin = flipped_xmin;
        isect.xmax = flipped_xmax;
      }
      if (mirror_y) {
        const float flipped_ymin = float(row + 1) - isect.ymax;
        const float flipped_ymax = float(row + 1) - isect.ymin;
        isect.ymin = flipped_ymin;
        isect.ymax = flipped_ymax;
      }
      BLI_rctf_do_minmax_v(&mirrored, float2(isect.xmin, isect.ymin));
      BLI_rctf_do_minmax_v(&mirrored, float2(isect.xmax, isect.ymax));
    }
  }
  return mirrored;
}

/** Rasterize \a shape into every UDIM tile whose UV square overlaps \a visit_uv_bounds. */
static void image_select_gesture_rasterize_tiles(const ImageUser &base_iuser,
                                                 Image *image,
                                                 const rctf &visit_uv_bounds,
                                                 const rctf &gesture_uv_bounds,
                                                 const ImageSelectGestureShape &shape,
                                                 const float fill_value,
                                                 const bool mirror_x,
                                                 const bool mirror_y,
                                                 const Span<Vector<float2>> symmetry_outlines)
{
  for (ImageTile *tile : ListBaseWrapper<ImageTile>(image->tiles)) {
    ImageUser iuser = base_iuser;
    iuser.tile = tile->tile_number;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &iuser, nullptr);
    if (!ibuf) {
      continue;
    }

    const float2 uv_origin = image_select_udim_tile_uv_origin(tile->tile_number);
    rctf visit_rect, gesture_rect;
    if (image_paint_selection_tile_intersection_get(uv_origin, visit_uv_bounds, &visit_rect)) {
      ImBuf *mask = BKE_image_paint_selection_mask_get(image, tile->tile_number, ibuf->x, ibuf->y);
      /* The original geometry is rasterized into every visited tile; the fill clips it to the
       * shape, so extra visits for the mirrored bounds are no-ops for tiles it does not reach. */
      if (image_paint_selection_tile_intersection_get(uv_origin, gesture_uv_bounds, &gesture_rect))
      {
        shape.rasterize_tile(uv_origin, gesture_rect, mask, fill_value);
      }
      if (mirror_x || mirror_y) {
        shape.rasterize_tile_mirrored(
            uv_origin, visit_rect, mask, fill_value, mirror_x, mirror_y);
      }
      /* Canvas symmetry copies are already mapped to UV; the fill clips them to the tile. */
      for (const Vector<float2> &outline : symmetry_outlines) {
        image_select_uv_polygon_rasterize_tile(uv_origin, outline, mask, fill_value);
      }
    }

    BKE_image_release_ibuf(image, ibuf, nullptr);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shared polygon rasterization
 * \{ */

/** Per-scanline sink for #BLI_bitmap_draw_2d_poly_v2i_n writing into a 1-channel float mask. */
struct MaskPolyFillData {
  float *data;
  int width;
  float fill_value;
};

static void mask_poly_fill_cb(int x, const int x_end, const int y, void *user_data)
{
  const MaskPolyFillData *fill = static_cast<const MaskPolyFillData *>(user_data);
  float *row = fill->data + int64_t(y) * fill->width;
  do {
    row[x] = fill->fill_value;
  } while (++x != x_end);
}

/**
 * Fill the interior of \a points into a 1-channel float buffer.
 *
 * Delegates to #BLI_bitmap_draw_2d_poly_v2i_n, which implements the even-odd rule with tracked
 * sorted spans. That handles self-intersecting lassos (a common freehand accident) correctly and
 * clips spans to the buffer, both of which the previous hand-rolled scanline fill got wrong.
 * Passing `(0, 0, width, height)` as the region makes the callback receive absolute buffer
 * coordinates, since the span callback reports coordinates relative to the region origin.
 */
static void fill_polygon_float(ImBuf *ibuf, const Span<int2> points, const float color)
{
  if (points.size() < 3) {
    return;
  }

  MaskPolyFillData fill{};
  fill.data = ibuf->float_data_for_write();
  fill.width = ibuf->x;
  fill.fill_value = color;

  BLI_bitmap_draw_2d_poly_v2i_n(0, 0, ibuf->x, ibuf->y, points, mask_poly_fill_cb, &fill);
}

void image_select_uv_polygon_rasterize_tile(const float2 &uv_origin,
                                            const Span<float2> uv_points,
                                            ImBuf *mask,
                                            const float fill_value,
                                            const bool mirror_x,
                                            const bool mirror_y)
{
  Vector<int2> tile_points;
  tile_points.reserve(uv_points.size());
  for (const float2 &uv : uv_points) {
    float px = (uv.x - uv_origin.x) * mask->x;
    float py = (uv.y - uv_origin.y) * mask->y;
    if (mirror_x) {
      px = float(mask->x) - px;
    }
    if (mirror_y) {
      py = float(mask->y) - py;
    }
    tile_points.append(int2(int(roundf(px)), int(roundf(py))));
  }
  fill_polygon_float(mask, tile_points, fill_value);
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
  const Vector<ImagePaintSelectionTarget> targets = image_paint_selection_targets_get(C, sima);

  /* A press-release without travel means "clear the selection", never "select nothing". */
  if (RNA_boolean_get(op->ptr, "is_simple_click")) {
    image_select_commit_floating_ops(C, sima);
    image_select_apply_deselect(C, targets);
    return OPERATOR_FINISHED;
  }

  /* Commit any floating move-selection fragment before starting a new selection. */
  image_select_commit_floating_ops(C, sima);

  rctf gesture_uv_bounds;
  if (!shape.uv_bounds_calc(region, op, gesture_uv_bounds)) {
    /* Degenerate gesture: behaves like a simple click. */
    image_select_apply_deselect(C, targets);
    return OPERATOR_FINISHED;
  }

  const eSelectOp sel_op = eSelectOp(RNA_enum_get(op->ptr, "mode"));
  const bool rasterize_gesture = image_paint_selection_should_rasterize_gesture(scene, sel_op);
  const float fill_value = (sel_op == SEL_OP_SUB) ? 0.0f : 1.0f;
  /* Canvas-space symmetry, driven by its own "affect selection" toggle. */
  const std::optional<ed::image_paint_symmetry::CanvasSymmetry> symmetry =
      ed::image_paint_symmetry::from_settings(*scene->toolsettings,
                                              IMAGE_PAINT_SYMMETRY_LINE_AFFECT_SELECTION);

  /* The canvas symmetry replaces the X/Y tile mirror: both at once would add a second, unrelated
   * copy of the gesture (the X/Y flags are shared with viewport painting, so they are often left
   * on). */
  bool mirror_x = false;
  bool mirror_y = false;
  if (!ed::image_paint_symmetry::canvas_mode_active(*scene->toolsettings)) {
    image_select_gesture_mirror_flags_get(scene, mirror_x, mirror_y);
  }
  const bool mirrored = mirror_x || mirror_y;

  /* Each symmetry copy of the outline, mapped once for every target and tile. Copies without a
   * finite image (a circle inversion around its own center) are dropped. */
  Vector<Vector<float2>> symmetry_outlines;
  Vector<rctf> symmetry_uv_bounds;
  if (symmetry) {
    const Vector<float2> outline = shape.uv_outline();
    for (const int copy : IndexRange(symmetry->copies_num())) {
      Vector<float2> mapped = ed::image_paint_symmetry::apply_polygon(*symmetry, copy, outline);
      if (mapped.size() < 3) {
        BKE_report(op->reports,
                   RPT_WARNING,
                   "Symmetry copy skipped: the selection encloses the circle center");
        continue;
      }
      rctf bounds;
      BLI_rctf_init_minmax(&bounds);
      for (const float2 &uv : mapped) {
        BLI_rctf_do_minmax_v(&bounds, uv);
      }
      symmetry_uv_bounds.append(bounds);
      symmetry_outlines.append(std::move(mapped));
    }
  }

  /* The copies may land on tiles the original gesture does not reach, so tiles are visited
   * against the union of all bounds. Face/island expansion only receives bounds, so each
   * transform gets its own expand pass below rather than one union, which would wrongly seed
   * faces between the gesture and its copies. */
  const rctf mirrored_uv_bounds = mirrored ?
                                       image_select_gesture_mirrored_bounds(
                                           gesture_uv_bounds, mirror_x, mirror_y) :
                                       gesture_uv_bounds;
  rctf visit_uv_bounds = gesture_uv_bounds;
  BLI_rctf_union(&visit_uv_bounds, &mirrored_uv_bounds);
  for (const rctf &bounds : symmetry_uv_bounds) {
    BLI_rctf_union(&visit_uv_bounds, &bounds);
  }

  /* One expand pass per transform of the gesture: the original, the tile mirror and each
   * symmetry copy. */
  const auto expand_all_transforms = [&]() {
    for (const ImagePaintSelectionTarget &target : targets) {
      image_paint_selection_expand(C, target.image, sel_op, &gesture_uv_bounds);
      if (mirrored) {
        image_paint_selection_expand(C, target.image, sel_op, &mirrored_uv_bounds);
      }
      for (const rctf &bounds : symmetry_uv_bounds) {
        image_paint_selection_expand(C, target.image, sel_op, &bounds);
      }
    }
  };

  image_paint_selection_undo_begin(shape.undo_name(), targets);

  /* Subtract: expand faces/islands before filling pixels so we seed from the gesture geometry,
   * not from remaining selected pixels (which would wrongly deselect unrelated islands). */
  if (sel_op == SEL_OP_SUB) {
    expand_all_transforms();
  }

  if (sel_op == SEL_OP_SET) {
    for (const ImagePaintSelectionTarget &target : targets) {
      BKE_image_paint_selection_mask_free(target.image);
    }
  }

  if (rasterize_gesture) {
    for (const ImagePaintSelectionTarget &target : targets) {
      image_select_gesture_rasterize_tiles(target.iuser,
                                           target.image,
                                           visit_uv_bounds,
                                           gesture_uv_bounds,
                                           shape,
                                           fill_value,
                                           mirror_x,
                                           mirror_y,
                                           symmetry_outlines);
    }
  }

  /* Add/set: expand to faces or UV islands touched by the gesture. */
  if (sel_op != SEL_OP_SUB) {
    expand_all_transforms();
  }

  ED_region_tag_redraw(region);

  for (const ImagePaintSelectionTarget &target : targets) {
    BKE_image_paint_selection_edge_policy_set(target.image, shape.edge_policy());
  }
  image_paint_selection_targets_update(C, targets);
  ED_image_undo_push_end();
  return OPERATOR_FINISHED;
}

/** \} */

}  // namespace blender
