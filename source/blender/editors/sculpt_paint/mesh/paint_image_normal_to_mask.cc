/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The Normal to Mask operators, moved out of the mask filter picker file they were born beside
 * (`paint_image_mask_filters.cc`) once they grew into their own session:
 * - Preview: the interactive pass over the active Normal to Mask mask item -- the Image Editor and
 *   the 3D Viewport show its grey, a drag moves the threshold, a click erases the connected region
 *   under the cursor into a painted mask item over it.
 * - Wire: the one-click wiring of the Anchor Point marker and the mask item that reads it.
 */

#include <algorithm>
#include <cmath>
#include <memory>

#include "BLT_translation.hh"

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_layers_tile.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "DEG_depsgraph.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "../paint_intern.hh"
#include "paint_image_mask_filters_intern.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Normal to Mask preview
 *
 * The interactive pass of a Normal to Mask mask item: the Image Editor (and the 3D Viewport, flat)
 * shows the grey the item lays at a preview resolution the field solve keeps up with, a drag moves
 * the threshold live, and a click erases the connected region under the cursor into a painted mask
 * item over it (Shift-click brings one back). What the session cannot reach the user cleans with
 * the brush in Mask mode, onto that same painted item.
 *
 * The preview computes its own grey from the slice the item reads -- the field solve over a
 * preview-sized copy of it, cached for the session, and the cut over that on every repaint --
 * rather than the mask effect cache's full-resolution one, so a drag that changes a parameter
 * repaints within a frame instead of re-solving the canvas. The row's own bake and composite
 * follow outside the session, as they do for any parameter edit.
 * \{ */

/** The longest side the preview renders at: the field solve and the repaint ride every hover and
 * drag step, and a solve this small stays within a frame. */
constexpr int NORMAL_TO_MASK_PREVIEW_MAX_SIDE = 512;

/** The distance in pixels a press travels before it is a threshold drag, not a click. */
constexpr float NORMAL_TO_MASK_DRAG_START = 8.0f;

struct NormalToMaskSession {
  /** The database the preview lives in, for the liveness test of #image_is_alive. */
  Main *bmain = nullptr;
  Material *ma = nullptr;
  /** The row the item hangs on, resolved once at the invoke and re-checked by the poll. */
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayer *item = nullptr;
  /** The painted mask item that erases regions (a Multiply over the item), or null until one
   * exists. */
  MaterialPaintLayer *cleanup = nullptr;
  Image *cleanup_map = nullptr;
  /** The slice the item reads, in the Normal channel, downsampled to the preview side: the buffer
   * the field solve runs over for every repaint. */
  ImBuf *slice = nullptr;
  /** The field the mode built from the slice, with the window it normalized by: cached across
   * repaints, so a hover or a threshold drag never reruns the solve -- what a drag recomputes is
   * the cut. Rebuilt when the mode changes; empty until the first repaint. */
  Vector<float> field;
  int field_mode = -1;
  float field_lo = 0.0f;
  float field_hi = 1.0f;
  /** The generated image the editors show: the item's grey with the cleanup laid over it, the
   * region under the cursor highlighted. */
  Image *preview = nullptr;
  int preview_width = 0;
  int preview_height = 0;
  DisplaySwap swap;
  bool has_hover = false;
  float2 hover_uv = {0.0f, 0.0f};
  /** The connected region under the cursor, one bit per preview texel, and its size. */
  Vector<bool> hover_component;
  int hover_component_size = 0;
  /** The press that may become a threshold drag, and where it started. */
  bool pressing = false;
  bool dragging = false;
  float drag_start_x = 0.0f;
  float drag_start_threshold = 0.0f;
  /** The parameters the preview was drawn for: a Properties edit repaints. */
  int drawn_mode = -1;
  float drawn_threshold = -1.0f;
  float drawn_softness = -1.0f;
  bool drawn_inverted = false;
};

/** Whether \a uv lands in the base tile the preview shows. */
static bool normal_to_mask_uv_in_tile(const float2 &uv)
{
  return uv.x >= 0.0f && uv.x < 1.0f && uv.y >= 0.0f && uv.y < 1.0f;
}

/** The UV under the cursor of the Image Editor or the 3D Viewport, in the base tile. */
static bool normal_to_mask_sample_uv(bContext *C, const wmEvent *event, float2 &r_uv)
{
  SpaceImage *sima = nullptr;
  if (const ARegion *region = image_region_at(C, event, &sima)) {
    r_uv = region_event_uv(*region, event);
  }
  else if (!view3d_event_uv(C, event, r_uv)) {
    return false;
  }
  return normal_to_mask_uv_in_tile(r_uv);
}

/** Whether \a event lands in an Image Editor or a 3D Viewport window region. */
static bool normal_to_mask_event_in_editors(bContext *C, const wmEvent *event)
{
  SpaceImage *sima = nullptr;
  return image_region_at(C, event, &sima) != nullptr || view3d_region_at(C, event) != nullptr;
}

/** The active mask item of the active material when it is a Normal to Mask, and its row. */
static bool normal_to_mask_target_get(bContext *C,
                                      Material **r_ma,
                                      MaterialPaintLayer **r_row,
                                      MaterialPaintLayer **r_item)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return false;
  }
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr) {
    return false;
  }
  MaterialPaintLayer *item = BKE_paint_layers_active_mask_item_get(*ma);
  if (item == nullptr || item->role != MA_PAINT_LAYER_ROLE_MASK_ITEM ||
      item->kind_subtype != MA_PAINT_LAYER_FILTER_NORMAL_TO_MASK)
  {
    return false;
  }
  MaterialPaintLayer *row = BKE_paint_layers_mask_item_owner(*ma, *item);
  if (row == nullptr) {
    return false;
  }
  *r_ma = ma;
  *r_row = row;
  *r_item = item;
  return true;
}

/** The painted mask item over the item that erases regions: the first painted one above it, or
 * null. A user who has stacked other items in between manages that stack by hand. */
static MaterialPaintLayer *normal_to_mask_cleanup_find(NormalToMaskSession &session)
{
  bool after = false;
  for (MaterialPaintLayer &candidate : session.row->mask_stack) {
    if (&candidate == session.item) {
      after = true;
      continue;
    }
    if (after && candidate.role == MA_PAINT_LAYER_ROLE_MASK_ITEM &&
        candidate.source == MA_PAINT_LAYER_SOURCE_IMAGE)
    {
      return &candidate;
    }
  }
  return nullptr;
}

/** The cleanup item, created when missing: a Paint mask correction, Multiply, laid right over the
 * item. Multiply because it serves both directions -- erasing paints black over a region (the mask
 * reads zero there) and restoring paints white (the item's own grey reads through unchanged). */
static MaterialPaintLayer *normal_to_mask_cleanup_ensure(NormalToMaskSession &session)
{
  if (MaterialPaintLayer *found = normal_to_mask_cleanup_find(session)) {
    return found;
  }
  MaterialPaintLayer *cleanup = BKE_paint_layers_mask_item_add(
      *session.ma, session.row, MA_PAINT_LAYER_SOURCE_IMAGE, "Normal to Mask Cleanup");
  if (cleanup == nullptr) {
    return nullptr;
  }
  cleanup->blend = MA_PAINT_LAYER_BLEND_MULTIPLY;
  BKE_paint_layers_reorder(
      *session.ma, cleanup, BLI_findindex(&session.row->mask_stack, session.item) + 1);
  BKE_paint_layers_tag_edited(*session.ma);
  return cleanup;
}

/** The map the cleanup item paints, created at the invoke so the clicks have somewhere to land.
 * The item is made active for the resolve and put back, so the session leaves the user's active
 * item as it found it. */
static Image *normal_to_mask_cleanup_map_ensure(Main &bmain, NormalToMaskSession &session)
{
  const bUUID restore = BKE_paint_layers_active_get(*session.ma);
  BKE_paint_layers_active_set(*session.ma, session.cleanup->marker);
  PaintLayersTarget target;
  Image *map = nullptr;
  if (BKE_paint_layers_target_get(*session.ma,
                                  PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                  PaintLayersTargetMode::Mask,
                                  target) &&
      target.mask_item == session.cleanup)
  {
    const int image_size = session.ma->paint_layers_image_size > 0 ?
                               session.ma->paint_layers_image_size :
                               1024;
    map = BKE_paint_layers_target_ensure_writable(bmain, target, image_size);
  }
  BKE_paint_layers_active_set(*session.ma, restore);
  return map;
}

/** The connected region of the kept texels that \a sx, \a sy seeds, four-connected. A seed off the
 * kept area fills nothing: the region a click erases is one the click stands in. */
static void normal_to_mask_component_fill(const int width,
                                          const int height,
                                          const Span<bool> kept,
                                          const int sx,
                                          const int sy,
                                          Vector<bool> &r_component)
{
  r_component = Vector<bool>(int64_t(width) * height, false);
  if (!kept[int64_t(sy) * width + sx]) {
    return;
  }
  Vector<int64_t> stack;
  stack.append(int64_t(sy) * width + sx);
  r_component[int64_t(sy) * width + sx] = true;
  while (!stack.is_empty()) {
    const int64_t index = stack.pop_last();
    const int x = int(index % width);
    const int y = int(index / width);
    const int neighbors[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
    for (const int (&step)[2] : neighbors) {
      if (step[0] < 0 || step[0] >= width || step[1] < 0 || step[1] >= height) {
        continue;
      }
      const int64_t next = int64_t(step[1]) * width + step[0];
      if (kept[next] && !r_component[next]) {
        r_component[next] = true;
        stack.append(next);
      }
    }
  }
}

/** Flood the region under \a uv over \a kept into the session's hover component, so the highlight
 * and the click that erases it read the same bits. */
static void normal_to_mask_hover_component_set(NormalToMaskSession &session,
                                               const Span<bool> kept,
                                               const float2 &uv)
{
  const int px = std::clamp(int(uv.x * float(session.preview_width)), 0, session.preview_width - 1);
  const int py = std::clamp(
      int(uv.y * float(session.preview_height)), 0, session.preview_height - 1);
  normal_to_mask_component_fill(
      session.preview_width, session.preview_height, kept, px, py, session.hover_component);
  session.hover_component_size = int(
      std::count(session.hover_component.begin(), session.hover_component.end(), true));
}

/** Drop the region highlight, as a drag or a cursor off the map leaves nothing under it. */
static void normal_to_mask_hover_component_clear(NormalToMaskSession &session)
{
  session.hover_component_size = 0;
  session.hover_component.clear();
}

/** Free the session and the preview-sized slice it owns. */
static void normal_to_mask_session_free(NormalToMaskSession *session)
{
  if (session->slice != nullptr) {
    IMB_freeImBuf(session->slice);
  }
  MEM_delete(session);
}

/** The buffers one repaint acquires, released on every exit: the early returns of a refresh must
 * not leak its preview-sized copy of the grey or drop an acquired image lock. */
struct NormalToMaskRefreshScope {
  NormalToMaskSession &session;
  /** The slice's copy the cut lays the item's grey over. */
  ImBuf *grey = nullptr;
  /** The cleanup map, acquired locked: the regions erased and restored so far. */
  ImBuf *cleanup_px = nullptr;
  void *cleanup_lock = nullptr;
  /** The preview image, acquired locked: the buffer the repaint writes. */
  ImBuf *dst = nullptr;
  void *dst_lock = nullptr;

  ~NormalToMaskRefreshScope()
  {
    if (dst != nullptr) {
      BKE_image_release_ibuf(session.preview, dst, dst_lock);
    }
    if (cleanup_px != nullptr) {
      BKE_image_release_ibuf(session.cleanup_map, cleanup_px, cleanup_lock);
    }
    if (grey != nullptr) {
      IMB_freeImBuf(grey);
    }
  }
};

/** Repaint the preview: the item's grey over the slice, the item's invert over that, the cleanup
 * map multiplied in, and the region under the cursor highlighted. The kept bits it leaves in \a
 * r_kept are what a click floods. */
static void normal_to_mask_preview_refresh(NormalToMaskSession &session, Vector<bool> &r_kept)
{
  PaintLayerNormalToMaskParams params;
  if (!BKE_paint_layers_normal_to_mask_params_get(*session.item, params)) {
    return;
  }
  const bool inverted = BKE_paint_layers_mask_item_is_inverted(*session.item);
  session.drawn_threshold = params.threshold;
  session.drawn_softness = params.softness;
  session.drawn_mode = params.mode;
  session.drawn_inverted = inverted;
  if (!image_is_alive(*session.bmain, session.preview) || session.slice == nullptr) {
    return;
  }
  /* The field is the solve: rebuilt only when the mode names a different one, so a hover or a
   * threshold drag costs the cut alone. A compute that cannot run (no float buffer) leaves the
   * cache empty and stamps nothing, so the next repaint tries again. */
  if (session.field_mode != params.mode || session.field.is_empty()) {
    BKE_paint_layers_normal_to_mask_field_compute(
        *session.slice, params, session.field, session.field_lo, session.field_hi);
    if (!session.field.is_empty()) {
      session.field_mode = params.mode;
    }
  }
  if (session.field.is_empty()) {
    return;
  }

  NormalToMaskRefreshScope scope{session};
  /* The cut over a copy of the slice: the slice stays the input the next repaint runs over. */
  scope.grey = IMB_dupImBuf(session.slice);
  if (scope.grey == nullptr || scope.grey->float_data() == nullptr) {
    return;
  }
  BKE_paint_layers_normal_to_mask_cut_apply(
      *scope.grey, session.field, session.field_lo, session.field_hi, params);
  const float *grey_px = scope.grey->float_data();
  const int gw = scope.grey->x;
  const int gh = scope.grey->y;

  /* The cleanup map: the regions erased and restored so far, sampled at the same UV. */
  ImageUser cleanup_iuser;
  if (session.cleanup_map != nullptr) {
    BKE_imageuser_default(&cleanup_iuser);
    cleanup_iuser.tile = PAINT_TILE_NUMBER_BASE;
    scope.cleanup_px = BKE_image_acquire_ibuf(
        session.cleanup_map, &cleanup_iuser, &scope.cleanup_lock);
  }
  const float cleanup_opacity = (session.cleanup != nullptr) ?
                                    BKE_paint_layers_effective_opacity(*session.cleanup) :
                                    1.0f;
  const bool cleanup_inverted = (session.cleanup != nullptr) ?
                                    BKE_paint_layers_mask_item_is_inverted(*session.cleanup) :
                                    false;

  scope.dst = BKE_image_acquire_ibuf(session.preview, nullptr, &scope.dst_lock);
  if (scope.dst == nullptr || scope.dst->byte_data_for_write() == nullptr) {
    return;
  }
  uchar *out = scope.dst->byte_data_for_write();
  const int pw = session.preview_width;
  const int ph = session.preview_height;
  r_kept.reinitialize(int64_t(pw) * ph);
  for (const int y : IndexRange(ph)) {
    const int gy = std::min(int(int64_t(y) * gh / ph), gh - 1);
    const int cleanup_height = scope.cleanup_px ? scope.cleanup_px->y : 1;
    const int cleanup_width = scope.cleanup_px ? scope.cleanup_px->x : 1;
    const int cy = std::min(int(int64_t(y) * cleanup_height / ph), cleanup_height - 1);
    for (const int x : IndexRange(pw)) {
      const int gx = std::min(int(int64_t(x) * gw / pw), gw - 1);
      const int cx = std::min(int(int64_t(x) * cleanup_width / pw), cleanup_width - 1);
      float shown = grey_px[int64_t(gy) * gw * 4 + gx * 4];
      shown = inverted ? 1.0f - shown : shown;
      if (scope.cleanup_px != nullptr && scope.cleanup_px->float_data() != nullptr) {
        const float *cleanup_texel = scope.cleanup_px->float_data() +
                                     (int64_t(cy) * scope.cleanup_px->x + cx) * 4;
        float cleanup = cleanup_texel[0];
        cleanup = cleanup_inverted ? 1.0f - cleanup : cleanup;
        shown *= cleanup * cleanup_opacity;
      }
      shown = std::clamp(shown, 0.0f, 1.0f);
      r_kept[int64_t(y) * pw + x] = shown > 0.5f;
      /* The kept area shows as white, the erased as black; the region under the cursor tinted, so
       * a click's reach is seen before it is taken. */
      float shade[3] = {shown, shown, shown};
      if (session.hover_component_size > 0 && !session.hover_component.is_empty() &&
          session.hover_component[int64_t(y) * pw + x])
      {
        shade[0] = shade[0] * 0.35f + 1.0f * 0.65f;
        shade[1] = shade[1] * 0.35f + 0.9f * 0.65f;
        shade[2] = shade[2] * 0.35f + 0.2f * 0.65f;
      }
      uchar *pixel = out + (int64_t(y) * pw + x) * 4;
      for (const int c : IndexRange(3)) {
        pixel[c] = uchar(std::clamp(shade[c], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      pixel[3] = 255;
    }
  }
  scope.dst->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(session.preview, scope.dst);
  BKE_image_partial_update_mark_full_update(session.preview);
}

/** Write \a fill into the cleanup map over the region under the cursor: black erases it from the
 * mask, white restores the item's own grey there. One image undo step per click. */
static void normal_to_mask_region_write(bContext *C,
                                        NormalToMaskSession &session,
                                        const float fill)
{
  if (session.cleanup_map == nullptr || session.hover_component_size <= 0 ||
      session.hover_component.is_empty())
  {
    return;
  }
  Image *map = session.cleanup_map;
  ImageUser undo_iuser;
  BKE_imageuser_default(&undo_iuser);
  ED_image_undo_push_begin_with_image_all_udims("Normal to Mask Cleanup", map, &undo_iuser);
  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  iuser.tile = PAINT_TILE_NUMBER_BASE;
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(map, &iuser, &lock);
  if (ibuf == nullptr) {
    ED_image_undo_push_end();
    return;
  }
  const int pw = session.preview_width;
  const int ph = session.preview_height;
  float *float_pixels = (ibuf->float_buffer.data != nullptr) ? ibuf->float_data_for_write() :
                                                               nullptr;
  uchar *byte_pixels = (float_pixels == nullptr && ibuf->byte_buffer.data != nullptr) ?
                           ibuf->byte_data_for_write() :
                           nullptr;
  /* The walk is over the map's own texels, each reading the preview texel it falls under: a map
   * larger than the preview paints every one of its texels the region covers, where a walk over
   * the preview's texels would leave one texel in every `map / preview` block untouched and erase
   * a dotted grid instead of a region. */
  for (const int y : IndexRange(ibuf->y)) {
    const int py = std::min(int(int64_t(y) * ph / ibuf->y), ph - 1);
    for (const int x : IndexRange(ibuf->x)) {
      const int px = std::min(int(int64_t(x) * pw / ibuf->x), pw - 1);
      if (!session.hover_component[int64_t(py) * pw + px]) {
        continue;
      }
      if (float_pixels != nullptr) {
        float *texel = float_pixels + (int64_t(y) * ibuf->x + x) * 4;
        texel[0] = texel[1] = texel[2] = fill;
        texel[3] = 1.0f;
      }
      else if (byte_pixels != nullptr) {
        uchar *texel = byte_pixels + (int64_t(y) * ibuf->x + x) * 4;
        texel[0] = texel[1] = texel[2] = uchar(fill * 255.0f + 0.5f);
        texel[3] = 255;
      }
    }
  }
  ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
  BKE_image_mark_dirty(map, ibuf);
  BKE_image_partial_update_mark_full_update(map);
  BKE_image_release_ibuf(map, ibuf, lock);
  ED_image_undo_push_end();
  DEG_id_tag_update(&session.ma->id, ID_RECALC_SHADING);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, session.ma);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
}

static void normal_to_mask_preview_end(bContext *C, wmOperator *op)
{
  NormalToMaskSession *session = static_cast<NormalToMaskSession *>(op->customdata);
  if (session != nullptr) {
    g_picker_image = nullptr;
    display_swap_restore(C, session->swap);
    if (image_is_alive(*session->bmain, session->preview)) {
      BKE_id_free(session->bmain, session->preview);
    }
    normal_to_mask_session_free(session);
    op->customdata = nullptr;
  }
  WM_cursor_modal_restore(CTX_wm_window(C));
  ED_workspace_status_text(C, nullptr);
  mask_filter_redraw_all(C);
}

static bool normal_to_mask_preview_poll(bContext *C)
{
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayer *item = nullptr;
  return normal_to_mask_target_get(C, &ma, &row, &item);
}

static wmOperatorStatus normal_to_mask_preview_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayer *item = nullptr;
  if (!normal_to_mask_target_get(C, &ma, &row, &item)) {
    return OPERATOR_CANCELLED;
  }
  /* The slice the item reads, at the row's canvas, brought down to the preview side: the buffer
   * every repaint runs the kernel over. A reference that does not resolve renders nothing. */
  std::shared_ptr<ImBuf> slice_coverage;
  PaintLayerSliceColor slice_color;
  if (!BKE_paint_layers_slice_render(*ma,
                                     *item,
                                     PAINT_MATERIAL_CHANNEL_NORMAL,
                                     &slice_color,
                                     &slice_coverage,
                                     nullptr,
                                     /*dimensions_only=*/false,
                                     PaintLayerTileContext{}))
  {
    BKE_report(op->reports,
               RPT_WARNING,
               "Set the item's Anchor Point source to a layer with a Normal first");
    return OPERATOR_CANCELLED;
  }
  /* The preview reads the slice per texel: a half slice never materializes whole. A byte-only
   * buffer (a shape the type allows, though today's render produces float or half) is read through
   * the same per-texel sampler instead of refused: its branch is straight, needs no decode. */
  int sw = 0;
  int sh = 0;
  paint_layer_slice_color_dimensions(slice_color, sw, sh);
  if (sw <= 0 || sh <= 0) {
    BKE_report(op->reports, RPT_WARNING, "The referenced layer lays no Normal content");
    return OPERATOR_CANCELLED;
  }

  NormalToMaskSession *session = MEM_new<NormalToMaskSession>(__func__);
  session->bmain = CTX_data_main(C);
  session->ma = ma;
  session->row = row;
  session->item = item;
  const int side = std::max(sw, sh);
  const float scale = (side > NORMAL_TO_MASK_PREVIEW_MAX_SIDE) ?
                          float(NORMAL_TO_MASK_PREVIEW_MAX_SIDE) / float(side) :
                          1.0f;
  session->preview_width = std::max(int(float(sw) * scale), 1);
  session->preview_height = std::max(int(float(sh) * scale), 1);
  session->slice = IMB_allocImBuf(
      session->preview_width, session->preview_height, ImBufFlags::FloatData);
  if (session->slice == nullptr || session->slice->float_data_for_write() == nullptr) {
    normal_to_mask_session_free(session);
    return OPERATOR_CANCELLED;
  }
  float *pixels = session->slice->float_data_for_write();
  for (const int y : IndexRange(session->preview_height)) {
    const int sy = std::min(int(int64_t(y) * sh / session->preview_height), sh - 1);
    for (const int x : IndexRange(session->preview_width)) {
      const int sx = std::min(int(int64_t(x) * sw / session->preview_width), sw - 1);
      float texel[4];
      paint_layer_slice_color_sample(slice_color, int64_t(sy) * sw + sx, texel);
      float *dst = pixels + (int64_t(y) * session->preview_width + x) * 4;
      dst[0] = texel[0];
      dst[1] = texel[1];
      dst[2] = texel[2];
      dst[3] = 1.0f;
    }
  }

  /* The cleanup item and its map, so the clicks have somewhere to land. A map that cannot be made
   * leaves the clicks out, but the threshold drag still runs. */
  session->cleanup = normal_to_mask_cleanup_ensure(*session);
  if (session->cleanup != nullptr) {
    session->cleanup_map = normal_to_mask_cleanup_map_ensure(*session->bmain, *session);
    if (session->cleanup_map == nullptr) {
      BKE_report(op->reports, RPT_WARNING, "The cleanup mask item cannot get a map to paint into");
    }
  }

  const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  session->preview = BKE_image_add_generated(CTX_data_main(C),
                                             session->preview_width,
                                             session->preview_height,
                                             "Normal to Mask Preview",
                                             32,
                                             false,
                                             IMA_GENTYPE_BLANK,
                                             black,
                                             false,
                                             true,
                                             false);
  if (session->preview == nullptr) {
    normal_to_mask_session_free(session);
    return OPERATOR_CANCELLED;
  }
  id_us_min(&session->preview->id);
  op->customdata = session;

  Vector<bool> kept;
  normal_to_mask_preview_refresh(*session, kept);
  display_swap_apply(C, session->swap, session->preview, true);
  g_picker_image = session->preview;

  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EYEDROPPER);
  ED_workspace_status_text(
      C,
      "Drag: threshold | Click: erase the region | Shift+Click: restore it | Esc/RMB/Enter: "
      "finish");
  WM_event_add_modal_handler(C, op);
  mask_filter_redraw_all(C);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus normal_to_mask_preview_modal(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent *event)
{
  NormalToMaskSession *session = static_cast<NormalToMaskSession *>(op->customdata);
  Material *ma = nullptr;
  MaterialPaintLayer *row = nullptr;
  MaterialPaintLayer *item = nullptr;
  if (!normal_to_mask_target_get(C, &ma, &row, &item) || item != session->item) {
    normal_to_mask_preview_end(C, op);
    return OPERATOR_CANCELLED;
  }

  bool consumed = false;
  bool repaint = false;

  /* The parameters live in the Properties editor: when they change, so does the preview. */
  PaintLayerNormalToMaskParams params;
  const bool params_known = BKE_paint_layers_normal_to_mask_params_get(*item, params);
  if (params_known &&
      (params.mode != session->drawn_mode ||
       fabsf(params.threshold - session->drawn_threshold) > 1e-6f ||
       fabsf(params.softness - session->drawn_softness) > 1e-6f ||
       BKE_paint_layers_mask_item_is_inverted(*item) != session->drawn_inverted))
  {
    repaint = true;
  }

  switch (event->type) {
    case EVT_ESCKEY:
    case RIGHTMOUSE:
    case EVT_RETKEY: {
      normal_to_mask_preview_end(C, op);
      return OPERATOR_FINISHED;
    }
    case LEFTMOUSE: {
      /* A press in the Image Editor or the 3D Viewport is the session's, over the map only: a
       * press that misses it goes on to the editor, like a click outside the map does. */
      if (event->val == KM_PRESS) {
        if (!normal_to_mask_event_in_editors(C, event)) {
          break;
        }
        float2 uv;
        if (!normal_to_mask_sample_uv(C, event, uv)) {
          break;
        }
        consumed = true;
        session->pressing = true;
        session->dragging = false;
        session->drag_start_x = float(event->xy[0]);
        session->drag_start_threshold = params_known ? params.threshold : 0.5f;
        session->hover_uv = uv;
      }
      else if (event->val == KM_RELEASE && session->pressing) {
        consumed = true;
        const bool was_dragging = session->dragging;
        session->pressing = false;
        session->dragging = false;
        if (!was_dragging && session->cleanup != nullptr) {
          /* A click: the region under the cursor is erased, or with Shift restored. */
          Vector<bool> kept;
          normal_to_mask_preview_refresh(*session, kept);
          normal_to_mask_hover_component_set(*session, kept, session->hover_uv);
          const float fill = (event->modifier & KM_SHIFT) != 0 ? 1.0f : 0.0f;
          normal_to_mask_region_write(C, *session, fill);
          repaint = true;
        }
        else if (was_dragging) {
          /* A drag is one threshold edit: its undo step lands here, so one Ctrl+Z takes the whole
           * travel back, and the Properties editor catches up once instead of per step -- the
           * steps only carried the value the cut reads. */
          WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, ma);
          ED_undo_push(C, "Normal to Mask Threshold");
        }
      }
      break;
    }
    case MOUSEMOVE: {
      float2 uv;
      const bool over_map = normal_to_mask_sample_uv(C, event, uv);
      if (session->pressing) {
        consumed = true;
        const float dx = float(event->xy[0]) - session->drag_start_x;
        if (!session->dragging && std::abs(dx) > NORMAL_TO_MASK_DRAG_START) {
          session->dragging = true;
          session->has_hover = false;
          normal_to_mask_hover_component_clear(*session);
        }
        if (session->dragging) {
          /* The drag maps its travel across the preview onto the whole threshold range: one sweep
           * walks the field from black to white. Each step writes the value the cut reads and
           * repaints at once; the material notifier and the undo step wait for the release, so a
           * drag is one edit rather than one per pixel of travel. */
          const float threshold = std::clamp(
              session->drag_start_threshold + dx / float(session->preview_width), 0.0f, 1.0f);
          if (std::abs(threshold - session->drawn_threshold) > 1e-4f) {
            BKE_paint_layers_kind_param_set(*ma, *item, "threshold", threshold);
            repaint = true;
          }
        }
        break;
      }
      /* A hover re-floods the region under the cursor, so the highlight follows it. */
      const bool changed = (over_map != session->has_hover) ||
                           (over_map && math::distance(uv, session->hover_uv) > 1e-4f);
      if (changed && over_map) {
        session->has_hover = true;
        session->hover_uv = uv;
        Vector<bool> kept;
        normal_to_mask_preview_refresh(*session, kept);
        normal_to_mask_hover_component_set(*session, kept, uv);
        normal_to_mask_preview_refresh(*session, kept);
        mask_filter_redraw_all(C);
      }
      else if (changed) {
        session->has_hover = false;
        normal_to_mask_hover_component_clear(*session);
        repaint = true;
      }
      break;
    }
    default:
      break;
  }

  if (repaint) {
    Vector<bool> kept;
    normal_to_mask_preview_refresh(*session, kept);
    mask_filter_redraw_all(C);
  }
  return consumed ? OPERATOR_RUNNING_MODAL : OPERATOR_PASS_THROUGH;
}

static void normal_to_mask_preview_cancel(bContext *C, wmOperator *op)
{
  normal_to_mask_preview_end(C, op);
}

void PAINT_OT_normal_to_mask_preview(wmOperatorType *ot)
{
  ot->name = "Preview Normal to Mask";
  ot->idname = "PAINT_OT_normal_to_mask_preview";
  ot->description =
      "Preview the active Normal to Mask mask item: the Image Editor and the 3D Viewport show its "
      "mask, a drag moves the threshold, and a click erases the connected region under the cursor "
      "into a painted mask item over it (Shift-click restores one)";

  ot->invoke = normal_to_mask_preview_invoke;
  ot->modal = normal_to_mask_preview_modal;
  ot->cancel = normal_to_mask_preview_cancel;
  ot->poll = normal_to_mask_preview_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Normal to Mask wire
 *
 * The one-click wiring of a Normal to Mask: the Anchor Point marker in the source row's effects,
 * and the mask item on the active row that reads it, laid over a base mask the row gets when it has
 * none. The source is picked from the rows of the material that take part in the Normal channel.
 * \{ */

/** The rows of \a ma that carry an enabled Normal record, in stack order: the sources a wire can
 * name. Folders take part through what they hold, so they are not listed. */
static Vector<MaterialPaintLayer *> normal_to_mask_sources_get(Material &ma)
{
  Vector<MaterialPaintLayer *> sources;
  BKE_paint_layers_foreach(ma,
                           [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
                             if (layer.role == MA_PAINT_LAYER_ROLE_LAYER &&
                                 BKE_paint_layers_mask_base(layer) == nullptr)
                             {
                               const MaterialPaintLayerChannel *record =
                                   BKE_paint_layers_row_channel_find(
                                       layer, PAINT_MATERIAL_CHANNEL_NORMAL);
                               if (record != nullptr &&
                                   record->state == MA_PAINT_LAYER_CHANNEL_ENABLED)
                               {
                                 sources.append(&layer);
                               }
                             }
                             return true;
                           });
  return sources;
}

static const EnumPropertyItem *normal_to_mask_source_itemf(bContext *C,
                                                           PointerRNA * /*ptr*/,
                                                           PropertyRNA * /*prop*/,
                                                           bool *r_free)
{
  Object *ob = CTX_data_active_object(C);
  Material *ma = (ob != nullptr) ? BKE_paint_layers_active_material_get(ob) : nullptr;
  if (ma == nullptr) {
    *r_free = false;
    return rna_enum_dummy_NULL_items;
  }
  const Vector<MaterialPaintLayer *> sources = normal_to_mask_sources_get(*ma);
  EnumPropertyItem *items = static_cast<EnumPropertyItem *>(
      MEM_new_array_uninitialized(sources.size() + 1, sizeof(EnumPropertyItem), __func__));
  int index = 0;
  for (const MaterialPaintLayer *source : sources) {
    /* The identifier points into the row's own name: it outlives the menu while the material
     * stands, and a copy would leak, as only the item array is freed with `r_free`. The exec reads
     * the index, so the identifier is a label here. */
    items[index] = {
        index, source->name, ICON_NORMALS_FACE, source->name, "Read this layer's normal map"};
    index++;
  }
  items[index] = {0, nullptr, 0, nullptr, nullptr};
  *r_free = true;
  return items;
}

static wmOperatorStatus normal_to_mask_wire_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  Material *ma = (ob != nullptr) ? BKE_paint_layers_active_material_get(ob) : nullptr;
  MaterialPaintLayer *target = (ma != nullptr) ? BKE_paint_layers_active_layer_get(*ma) : nullptr;
  if (ma == nullptr || target == nullptr) {
    return OPERATOR_CANCELLED;
  }
  const Vector<MaterialPaintLayer *> sources = normal_to_mask_sources_get(*ma);
  const int source_index = RNA_enum_get(op->ptr, "source");
  if (source_index < 0 || source_index >= int(sources.size())) {
    BKE_report(op->reports, RPT_WARNING, "Pick a layer with a Normal to read from");
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *item = BKE_paint_layers_normal_to_mask_add(
      *ma, target, sources[source_index]);
  if (item == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The Normal to Mask could not be wired");
    return OPERATOR_CANCELLED;
  }
  BKE_paint_layers_active_set(*ma, item->marker);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, ma);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_PROPERTIES, nullptr);
  return OPERATOR_FINISHED;
}

static bool normal_to_mask_wire_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  Material *ma = (ob != nullptr) ? BKE_paint_layers_active_material_get(ob) : nullptr;
  if (ma == nullptr) {
    return false;
  }
  MaterialPaintLayer *target = BKE_paint_layers_active_layer_get(*ma);
  if (target == nullptr) {
    return false;
  }
  /* The active row is the mask's owner and needs no Normal of its own: what the entry promises is
   * that the dialog has a source to name, the same rule the source picker lists rows by. */
  return !normal_to_mask_sources_get(*ma).is_empty();
}

/** The dialog names the source: an exec alone would wire the list's first row silently, and the
 * poll only promises that a Normal exists, not which one the mask should read. */
static wmOperatorStatus normal_to_mask_wire_invoke(bContext *C,
                                                   wmOperator *op,
                                                   const wmEvent * /*event*/)
{
  return WM_operator_props_dialog_popup(C, op, 320, std::nullopt, IFACE_("Add"));
}

void PAINT_OT_normal_to_mask_wire(wmOperatorType *ot)
{
  ot->name = "Add Normal to Mask";
  ot->idname = "PAINT_OT_normal_to_mask_wire";
  ot->description =
      "Turn another layer's normal map into this layer's mask: an Anchor Point marker and a Normal "
      "to Mask mask item are wired together over the layer the dialog names";

  ot->invoke = normal_to_mask_wire_invoke;
  ot->exec = normal_to_mask_wire_exec;
  ot->poll = normal_to_mask_wire_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna,
                          "source",
                          rna_enum_dummy_NULL_items,
                          0,
                          "Source Layer",
                          "The layer whose normal map the mask is built from");
  RNA_def_enum_funcs(ot->prop, normal_to_mask_source_itemf);
}

/** \} */

}  // namespace blender
