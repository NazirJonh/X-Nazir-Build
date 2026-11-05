/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <algorithm>
#include <optional>

#include "BLI_index_range.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_base.h"
#include "BLI_math_color_blend.h"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_paint_selection.hh"
#include "BKE_library.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers_target.hh"
/* Full #PaintMode definition: #BKE_paint.hh only forward-declares the enum. */
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_intern.hh"
#include "paint_image_select_intern.hh"
/* The 3D Viewport's twin of the canvas target resolver, for Delete Selection in Sculpt Mode. */
#include "paint_image_select_view3d.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Delete Selection
 *
 * The Del-key cut of the selection tools: every target map of the edited row (the Paint layer's
 * channels, a Paint correction's, or the Mask / mask-correction map) loses the selected pixels the
 * way an Erase brush stroke would -- coverage cleared with the selection's own blend weight, so a
 * feathered edge fades instead of guillotining, and an opaque mask map (a Black/White Mask) is
 * restored to its generated fill exactly like the brush's mask-erase branch. The selection itself
 * stays: delete is a raster edit, not a deselect.
 * \{ */

/**
 * Display bookkeeping after a tile's pixels changed: the Image Editor re-reads the buffer
 * (display-buffer invalidation), dependent views re-decode the touched region (partial update).
 * The per-image half (cached GPU textures, ID tag) is #image_select_delete_image_finalize.
 */
static void image_select_delete_tile_finalize(Image &image,
                                              const int tile_number,
                                              ImBuf &ibuf,
                                              const rcti &region)
{
  ibuf.userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(&image, &ibuf);
  if (const ImageTile *image_tile = BKE_image_get_tile(&image, tile_number)) {
    BKE_image_partial_update_mark_region(&image, image_tile, &ibuf, &region);
  }
  else {
    BKE_image_partial_update_mark_full_update(&image);
  }
}

/**
 * Per-image half of the finalize: the canvas is displayed through cached GPU textures (the 3D
 * paint material and the Image Editor), so they are dropped to force a reload from the modified
 * ibufs. Same pair the commit paths of move / transform / warp apply.
 */
static void image_select_delete_image_finalize(Image &image)
{
  BKE_image_free_gputextures(&image);
  DEG_id_tag_update(&image.id, 0);
}

/**
 * Clear \a region of \a ibuf with the selection weights of the tile, in place.
 * Float buffers blend through #blend_color_erase_alpha_float and byte buffers through
 * #blend_color_erase_alpha_byte -- the exact pair #IMB_rectblend applies for an Erase brush
 * stroke (premultiplied float, straight byte), so a delete matches the brush on every coverage
 * level, including its near-zero floor. A mask map with an opaque generated fill (\a mask_fill)
 * takes the brush's Mix-to-fill branch instead: coverage is restored toward the fill, feathered by
 * the selection weight.
 */
static void image_select_delete_erase_region(Image &image,
                                             ImBuf &ibuf,
                                             const int tile_number,
                                             const rcti &region,
                                             const std::optional<float4> &mask_fill)
{
  if (float *data = ibuf.float_data_for_write()) {
    const int channels = ibuf.channels ? ibuf.channels : 4;
    const bool has_alpha = channels == 4;
    image_select_foreach_weighted_row(
        image, tile_number, region, [&](const int y, const Span<float> weights) {
          float *row = data + size_t(y) * ibuf.x * channels;
          for (const int x : IndexRange::from_begin_end(region.xmin, region.xmax)) {
            const float weight = weights.is_empty() ? 1.0f : weights[x - region.xmin];
            if (weight <= 0.0f) {
              continue;
            }
            float *px = row + size_t(x) * channels;
            if (mask_fill) {
              /* Mix toward the generated fill, feathered by the selection weight. */
              for (const int c : IndexRange(min_ii(channels, 3))) {
                px[c] = px[c] + ((*mask_fill)[c] - px[c]) * weight;
              }
              if (has_alpha) {
                px[3] = px[3] + ((*mask_fill)[3] - px[3]) * weight;
              }
              continue;
            }
            /* The selection weight is the brush alpha, the pixel the canvas: buffers with fewer
             * than four channels keep the implicit full alpha of a colorless map. The blend works
             * on RGBA, so wider maps keep the channels past alpha untouched. */
            float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (const int c : IndexRange(min_ii(channels, 4))) {
              rgba[c] = px[c];
            }
            const float erase_color[4] = {0.0f, 0.0f, 0.0f, weight};
            float erased[4];
            blend_color_erase_alpha_float(erased, rgba, erase_color);
            for (const int c : IndexRange(min_ii(channels, 4))) {
              px[c] = erased[c];
            }
          }
        });
    return;
  }

  uint8_t *data = ibuf.byte_data_for_write();
  if (data == nullptr) {
    return;
  }
  image_select_foreach_weighted_row(
      image, tile_number, region, [&](const int y, const Span<float> weights) {
        uint8_t *row = data + size_t(y) * ibuf.x * 4;
        for (const int x : IndexRange::from_begin_end(region.xmin, region.xmax)) {
          const float weight = weights.is_empty() ? 1.0f : weights[x - region.xmin];
          if (weight <= 0.0f) {
            continue;
          }
          uint8_t *px = row + size_t(x) * 4;
          const int t = min_ii(int(weight * 255.0f + 0.5f), 255);
          if (mask_fill) {
            for (const int c : IndexRange(4)) {
              const int fill_c = clamp_i(int((*mask_fill)[c] * 255.0f + 0.5f), 0, 255);
              px[c] = uchar(px[c] + (fill_c - px[c]) * t / 255);
            }
            continue;
          }
          /* Same call #IMB_rectblend makes for an Erase stroke on byte buffers; in-place is the
           * aliasing it is written for. */
          const uint8_t erase_color[4] = {0, 0, 0, uint8_t(t)};
          blend_color_erase_alpha_byte(px, px, erase_color);
        }
      });
}

/** Tiles of a target that carry a selection, each with its erase region (feather included). */
struct ImageSelectDeleteTile {
  int tile_number;
  rcti region;
};

/** A target with something to delete, resolved once so the undo and erase passes agree. */
struct ImageSelectDeletePending {
  ImagePaintSelectionTarget target;
  Vector<ImageSelectDeleteTile> tiles;
  /** Set for a mask map with an opaque generated fill: erase restores it instead of clearing. */
  std::optional<float4> mask_fill;
};

static Vector<ImageSelectDeleteTile> image_select_delete_target_tiles(
    const ImagePaintSelectionTarget &target)
{
  Vector<ImageSelectDeleteTile> tiles;
  const PaintSelectionEdgePolicy &edge_policy = BKE_image_paint_selection_edge_policy_get(
      target.image);
  for (const ImageTile *tile : ConstListBaseWrapper<ImageTile>(target.image->tiles)) {
    int min_px[2];
    int max_px[2];
    if (!BKE_image_paint_selection_mask_bounds(target.image, tile->tile_number, min_px, max_px)) {
      continue;
    }
    const ImageSelectTileBuf buf(*target.image, target.iuser, tile->tile_number);
    const ImBuf *ibuf = buf.get();
    if (ibuf == nullptr) {
      continue;
    }
    /* Grow with the outward feather, clamped to the tile. The bounds stay exclusive on the
     * upper corners through the expansion, so the region below reads [xmin, xmax) x
     * [ymin, ymax) and #BLI_rcti_size_x keeps matching the pixel count. */
    BKE_image_paint_selection_bounds_expand_for_blend(
        min_px, max_px, ibuf->x, ibuf->y, edge_policy);
    if (min_px[0] >= max_px[0] || min_px[1] >= max_px[1]) {
      continue;
    }
    rcti region;
    BLI_rcti_init(&region, min_px[0], max_px[0], min_px[1], max_px[1]);
    tiles.append(ImageSelectDeleteTile{tile->tile_number, region});
  }
  return tiles;
}

/** \return True when any target's image carries a selection to delete. */
static bool image_select_delete_targets_have_selection(
    const Span<ImagePaintSelectionTarget> targets)
{
  return std::any_of(targets.begin(), targets.end(), [](const ImagePaintSelectionTarget &target) {
    return BKE_image_paint_selection_mask_has_any(target.image);
  });
}

/** Canvas targets of the context: the Image Editor's, else the active object's Sculpt canvas. */
static Vector<ImagePaintSelectionTarget> image_select_delete_targets_get(bContext *C)
{
  if (SpaceImage *sima = CTX_wm_space_image(C)) {
    if (sima->image == nullptr) {
      return {};
    }
    return image_paint_selection_targets_get(C, sima);
  }
  if (Object *ob = CTX_data_active_object(C)) {
    return blender::ed::sculpt_paint::image_paint_selection_view3d_targets_get(C, *ob);
  }
  return {};
}

static wmOperatorStatus image_select_delete_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return OPERATOR_CANCELLED;
  }
  ToolSettings &settings = *scene->toolsettings;

  /* Every target of a Mask-mode row IS the mask map: the resolvers name it through Base Color
   * and refuse the row's other channels there. The shared BKE rule still decides per target, so
   * a Content-mode target or a mask whose generated fill is not opaque takes the plain erase. */
  const bool mask_mode = BKE_paint_layers_target_is_mask(settings.paint_mode);

  Vector<ImageSelectDeletePending> pending;
  const Vector<ImagePaintSelectionTarget> targets = image_select_delete_targets_get(C);
  for (const ImagePaintSelectionTarget &target : targets) {
    Vector<ImageSelectDeleteTile> tiles = image_select_delete_target_tiles(target);
    if (tiles.is_empty()) {
      continue;
    }
    ImageSelectDeletePending item;
    item.target = target;
    item.tiles = std::move(tiles);
    float fill[4];
    if (BKE_paint_is_mask_target(mask_mode, target.channel) &&
        BKE_paint_mask_erase_fill_get(target.image, fill))
    {
      item.mask_fill = float4(fill);
    }
    pending.append(std::move(item));
  }

  if (pending.is_empty()) {
    BKE_report(op->reports, RPT_WARNING, "Nothing selected to delete");
    return OPERATOR_CANCELLED;
  }

  /* One image-undo step for the whole operation: the first touched tile anchors it -- registered
   * right here by #ED_image_undo_push_begin_with_image -- and every other touched tile registers
   * its "before" pixels into the same step (the shared select-tools session; selection masks are
   * not captured here because the masks are left intact). */
  ED_imapaint_clear_partial_redraw();
  const ImageSelectDeletePending &anchor = pending.first();
  const int anchor_tile_number = anchor.tiles[0].tile_number;
  bool anchor_registered = false;
  {
    ImageSelectTileBuf anchor_buf(*anchor.target.image, anchor.target.iuser, anchor_tile_number);
    if (anchor_buf.get() != nullptr) {
      ED_image_undo_push_begin_with_image(
          "Delete Selection", anchor.target.image, anchor_buf.get(), &anchor_buf.tile_iuser());
      anchor_registered = true;
    }
    else {
      ED_image_undo_push_begin("Delete Selection", PaintMode::Texture2D);
    }
  }
  ImageUndoStep *us_open = image_select_undo_session_step_get();

  for (const ImageSelectDeletePending &item : pending) {
    for (const ImageSelectDeleteTile &tile : item.tiles) {
      ImageSelectTileBuf buf(*item.target.image, item.target.iuser, tile.tile_number);
      ImBuf *ibuf = buf.get();
      if (ibuf == nullptr ||
          (ibuf->float_buffer.data == nullptr && ibuf->byte_buffer.data == nullptr))
      {
        continue;
      }
      /* The anchor tile captured its "before" pixels at #ED_image_undo_push_begin_with_image; a
       * second push would overwrite that capture (or trip the fresh-buffer assert in
       * #ubuf_from_image_all_tiles). */
      const bool is_anchor = anchor_registered && item.target.image == anchor.target.image &&
                             tile.tile_number == anchor_tile_number;
      if (us_open != nullptr && !is_anchor) {
        ED_image_undo_push(item.target.image, ibuf, &buf.tile_iuser(), us_open);
      }
      image_select_delete_erase_region(
          *item.target.image, *ibuf, tile.tile_number, tile.region, item.mask_fill);
      image_select_delete_tile_finalize(*item.target.image, tile.tile_number, *ibuf, tile.region);
    }
    image_select_delete_image_finalize(*item.target.image);
  }

  ED_image_undo_push_end();

  /* The shared select-tools epilogue: per-target notifiers plus the window-level one and the
   * scene ID tags that make the Sculpt viewport reload its paint textures. A bare NC_IMAGE on
   * the first target never reached the 3D editor. */
  Vector<ImagePaintSelectionTarget> touched_targets;
  touched_targets.reserve(pending.size());
  for (const ImageSelectDeletePending &item : pending) {
    touched_targets.append(item.target);
  }
  image_paint_selection_targets_update(C, touched_targets);

  return OPERATOR_FINISHED;
}

static bool image_select_delete_poll(bContext *C)
{
  /* Cheap context checks first: the poll runs on every Del press and every Select menu redraw,
   * and only then pays for resolving the canvas targets. */
  if (CTX_wm_space_image(C) != nullptr) {
    /* #image_paint_selection_poll already refuses while any select-tool session floats. */
    if (!image_paint_selection_poll(C)) {
      return false;
    }
    const Vector<ImagePaintSelectionTarget> targets = image_select_delete_targets_get(C);
    const bool result = image_select_delete_targets_have_selection(targets);
    return result;
  }
  else {
    /* The 3D counterpart of the same gesture: the Sculpt-mode paint session, with a selection on
     * the canvas the object paints into. Like the Image Editor, it does not need a select tool to be
     * active: the selection outlives the tool, and the poll only passes while one exists. */
    if (CTX_data_mode_enum(C) != CTX_MODE_SCULPT ||
        blender::ed::sculpt_paint::image_select_view3d_session_active() != nullptr ||
        CTX_data_active_object(C) == nullptr ||
        !blender::ed::sculpt_paint::image_paint_selection_view3d_poll(C))
    {
      return false;
    }
  }
  return image_select_delete_targets_have_selection(image_select_delete_targets_get(C));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator type registrations
 * \{ */

void PAINT_OT_image_select_delete(wmOperatorType *ot)
{
  ot->name = "Delete Selection";
  ot->idname = "PAINT_OT_image_select_delete";
  ot->description =
      "Erase the selected region from every channel map of the edited layer, like an Erase stroke";

  ot->exec = image_select_delete_exec;
  ot->poll = image_select_delete_poll;

  /* OPTYPE_UNDO stays off: exec opens and closes its own image-undo step (the select tools'
   * shared session), and a second WM step around this operator would describe nothing. */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

} /* namespace blender */
