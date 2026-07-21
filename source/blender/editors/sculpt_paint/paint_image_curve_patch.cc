/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the 2D Image Editor Curve Patch session.
 *
 * The session is a single in-flight object stored in a file-static -- the modal operator
 * (Stage 7) and the anchor stroke (Stage 6) both look it up via
 * #image_curve_patch_session_active_get(). Three lifecycle transitions are supported
 * explicitly:
 *
 * - begin: snapshot frozen parameters, open owned image-undo step, register globally.
 * - restore_and_restamp: restore the image to the anchor state, rebuild the patch geometry from
 *   the canonical curve, and rasterize it directly (`paint_image_curve_patch_raster.hh`).
 * - commit / cancel: draw a final-quality pass (commit only) and either commit or abort the
 *   owned image undo step.
 *
 * The pixel writer replaces the dab-replay path (`paint_2d_stroke()`) this file used to drive --
 * see `.MyTaskAndDoc/Paint-Curve-in-Sculpt-Mode/2026-08-20-image-curve-patch-rasterizer-spec.md`.
 */

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_image.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_undo_system.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_paint.hh"
#include "ED_paint_curve_draw.hh"

#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"
#include "WM_api.hh"

#include "UI_view2d.hh"

#include "paint_image_curve_patch.hh"
#include "paint_image_curve_patch_raster.hh"

namespace blender {

bool get_imapaint_zoom(bContext *C, float *zoomx, float *zoomy);

namespace ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Active Session Singleton
 * \{ */

/* Single in-flight 2D Curve Patch session. The modal operator's poll (Stage 7) and the
 * anchor-stroke integration (Stage 6) both look this up; both treat "active" as a precondition
 * for their own work. */
static ImageCurvePatchSession *g_active_session = nullptr;

bool image_curve_patch_session_active()
{
  return g_active_session != nullptr;
}

ImageCurvePatchSession *image_curve_patch_session_active_get()
{
  return g_active_session;
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Frozen Parameter Snapshot
 * \{ */

static ImageCurvePatchFrozenParams image_curve_patch_capture_params(const Brush *brush,
                                                                    const Paint *paint)
{
  ImageCurvePatchFrozenParams params;
  params.alpha = BKE_brush_alpha_get(paint, brush);
  params.blend = brush->blend;
  params.brush_type = brush->image_brush_type;

  const float3 rgb = BKE_brush_color_get(paint, brush);
  copy_v3_v3(params.color, rgb);
  params.color[3] = params.alpha;
  return params;
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Lifecycle
 * \{ */

ImageCurvePatchSession::~ImageCurvePatchSession()
{
  if (tex_pool != nullptr) {
    BKE_image_pool_free(tex_pool);
  }
  if (tiles != nullptr) {
    ED_image_paint_tile_map_free(tiles);
    tiles = nullptr;
  }
}

ImagePool &ImageCurvePatchSession::tex_pool_ensure()
{
  if (tex_pool == nullptr) {
    tex_pool = BKE_image_pool_new();
  }
  return *tex_pool;
}

void ImageCurvePatchSession::tex_pool_invalidate()
{
  if (tex_pool != nullptr) {
    BKE_image_pool_free(tex_pool);
    tex_pool = nullptr;
  }
}

ImageCurvePatchSession *image_curve_patch_session_begin(bContext *C,
                                                        ReportList *reports,
                                                        const bke::CurvesGeometry &initial_curve,
                                                        Brush *brush,
                                                        const Paint *paint)
{
  /* Spec §7.3 ("Determinism of replay") limits the supported brush types to baseline DRAW.
   * Anything else would force us to snapshot more state (clone offsets, smear history, fill
   * thresholds) and isn't worth carrying in Stage 5 -- the modal operator's poll will reject it
   * later, but the anchor stroke should also surface a clear report at the source. */
  if (brush == nullptr || brush->image_brush_type != IMAGE_PAINT_BRUSH_TYPE_DRAW) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Curve Patch supports only Draw brushes (got %d)",
                brush == nullptr ? -1 : int(brush->image_brush_type));
    return nullptr;
  }
  if (g_active_session != nullptr) {
    /* Nested Curve Patch sessions are not allowed (only one modal edit at a time per
     * editor). The pre-existing session has to be disposed first; if its owning operator is
     * gone, drop it without ceremony. */
    if (g_active_session->tiles != nullptr) {
      ED_image_paint_tile_map_restore(g_active_session->tiles);
    }
    MEM_delete(g_active_session);
    g_active_session = nullptr;
  }
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->image == nullptr) {
    BKE_report(reports, RPT_ERROR, "Curve Patch requires an Image Editor with an open image");
    return nullptr;
  }

  ImageUser iuser = {};
  iuser.framenr = sima->iuser.framenr;
  const ImBuf *ibuf = BKE_image_acquire_ibuf(sima->image, &iuser, nullptr);
  const bool has_four_channels = ibuf != nullptr && ibuf->channels == 4;
  BKE_image_release_ibuf(sima->image, const_cast<ImBuf *>(ibuf), nullptr);
  if (!has_four_channels) {
    BKE_report(reports, RPT_WARNING, "Image requires 4 color channels to paint");
    return nullptr;
  }

  /* MEM_new, NOT `std::make_unique`: every path that ends this session frees it with #MEM_delete
   * (#image_curve_patch_session_commit, #image_curve_patch_session_cancel, and the nested-session
   * branch above), and handing a pointer from the global `operator new` to guardedalloc makes it
   * read whatever lies in front of the object as a `MemHead` -- the crash lands inside
   * `MEM_lockfree_freeN` on an address derived from that garbage. There is nothing for a
   * `unique_ptr` to guard here anyway: every failure exit is above this line, so the object cannot
   * leak between its construction and the assignment to `g_active_session` below. */
  ImageCurvePatchSession *session = MEM_new<ImageCurvePatchSession>(__func__);
  session->curve = initial_curve;
  /* The canvas has exactly one patch; every consumer reaches it through `doc.active_item()`. */
  session->doc.patches.resize(1);
  session->params = image_curve_patch_capture_params(brush, paint);
  /* The session's own "before" pixels. Nothing is pushed onto the undo stack until commit, so no
   * foreign operator can take a transaction out from under a patch that is still being edited. */
  session->tiles = ED_image_paint_tile_map_new();
  session->image = sima->image;

  float zoomx = 1.0f, zoomy = 1.0f;
  get_imapaint_zoom(C, &zoomx, &zoomy);
  session->zoom_2d = std::max(zoomx, zoomy);
  const float pixel_radius = BKE_brush_radius_get(paint, brush);
  session->radius_per_size = (pixel_radius / session->zoom_2d) /
                             float(std::max(1, BKE_brush_size_get(paint, brush)));

  g_active_session = session;
  return g_active_session;
}

/* Roll the canvas back to the pre-patch state captured in the session's own tile map. The
 * partial-update notification the Image Editor's `PartialUpdateChecker` needs is issued by the
 * helper itself, for every image the map covers -- including the UDIM tiles this session never
 * resolved an `Image *` for. */
static void curve_patch_restore_to_anchor(ImageCurvePatchSession &session)
{
  if (session.tiles != nullptr) {
    ED_image_paint_tile_map_restore(session.tiles);
  }
}

void image_curve_patch_session_commit(bContext *C, ImageCurvePatchSession *session)
{
  BLI_assert(session == g_active_session);
  if (session == nullptr) {
    return;
  }
  if (session->tiles != nullptr) {
    /* Restore first, then re-stamp at final quality, and only then hand the captured tiles to the
     * undo system: the restore invalidates every tile, the final stamp re-validates exactly the
     * ones it touches, and the history entry ends up describing precisely what the canvas keeps.
     */
    curve_patch_restore_to_anchor(*session);
    image_curve_patch_raster_draw_final(C, *session);
    ED_image_undo_push_from_tile_map("Curve Patch", PaintMode::Texture2D, session->tiles);
  }
  g_active_session = nullptr;
  MEM_delete(session);
}

void image_curve_patch_session_cancel(bContext * /*C*/, ImageCurvePatchSession *session)
{
  BLI_assert(session == g_active_session);
  if (session == nullptr) {
    return;
  }
  /* Put the pristine pixels back and write no history entry at all: nothing was ever pushed onto
   * the undo stack for this session, so there is nothing to discard either. */
  curve_patch_restore_to_anchor(*session);
  g_active_session = nullptr;
  MEM_delete(session);
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Restore & Re-stamp
 * \{ */

/* #curve_patch_live_inputs_capture plus the one live input only the pixel writer has, and the
 * resolved build parameters the caller compares alongside it. `final_quality` is cleared: the
 * commit path sets it on the frozen params and never clears it, so leaving it in the compare
 * would make the watchdog see a phantom change. */
static CurvePatchLiveInputs image_curve_patch_live_inputs_capture(ImageCurvePatchSession &session,
                                                                  const Paint &paint,
                                                                  const Brush &brush,
                                                                  bke::CurvePatchParams &r_params)
{
  r_params = image_curve_patch_params_resolve(session, paint, brush);
  r_params.final_quality = false;

  CurvePatchLiveInputs in = curve_patch_live_inputs_capture(paint, brush);
  in.blend = brush.blend;
  return in;
}

bool image_curve_patch_session_sync_live_brush(bContext *C, ImageCurvePatchSession *session)
{
  if (session == nullptr) {
    return false;
  }
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = paint ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush == nullptr) {
    return false;
  }

  bke::CurvePatchParams live_params;
  const CurvePatchLiveInputs live = image_curve_patch_live_inputs_capture(
      *session, *paint, *brush, live_params);
  if (live == session->doc.last_synced && live_params == session->doc.last_synced_params) {
    return false;
  }

  if (live.needs_texture_pool_rebuild(session->doc.last_synced)) {
    session->tex_pool_invalidate();
  }
  /* Fields the writer reads straight off the session rather than off the brush. Keeping them in
   * step here is what makes a color or strength edit visible without ending the session. */
  session->params.alpha = live.alpha;
  session->params.blend = brush->blend;
  copy_v3_v3(session->params.color, live.brush_color);
  session->params.color[3] = live.alpha;

  /* Seeds `last_synced` on the way out, so an unchanged brush does not re-stamp forever. */
  image_curve_patch_session_restore_and_restamp(C, session);
  return true;
}

void image_curve_patch_session_restore_and_restamp(bContext *C, ImageCurvePatchSession *session)
{
  BLI_assert(session == g_active_session);
  if (session == nullptr) {
    return;
  }
  curve_patch_restore_to_anchor(*session);
  image_curve_patch_geometry_rebuild(C, *session);
  image_curve_patch_raster_draw(C, *session);

  /* Record the brush state this stamp was built from. Every path that puts pixels on the canvas
   * goes through here, so this is the one place the live-brush watchdog's baseline can be kept
   * honest -- seeding it anywhere else would leave the first poll comparing against defaults and
   * re-stamping for no reason. */
  if (const Paint *paint = BKE_paint_get_active_from_context(C)) {
    if (const Brush *brush = BKE_paint_brush_for_read(paint)) {
      session->doc.last_synced = image_curve_patch_live_inputs_capture(
          *session, *paint, *brush, session->doc.last_synced_params);
    }
  }
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Screen Adapter
 * \{ */

void image_curve_patch_region_to_uv(bContext *C, const int region_mval[2], float r_uv[2])
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    r_uv[0] = r_uv[1] = 0.0f;
    return;
  }
  /* Region-local coordinates (`#wmEvent::mval`), the space `View2D` maps from -- the same
   * convention `paint_2d_stroke` uses for its `mval[2]`. */
  ui::view2d_region_to_view(
      &region->v2d, float(region_mval[0]), float(region_mval[1]), &r_uv[0], &r_uv[1]);
}

bool ED_image_curve_patch_overlay_geometry_get(const ARegion *region,
                                               bke::CurvesGeometry &r_geometry)
{
  const ImageCurvePatchSession *session = g_active_session;
  if (session == nullptr || region == nullptr) {
    return false;
  }
  const bke::CurvesGeometry &src = session->curve;
  if (src.points_num() == 0 || src.curves_num() == 0) {
    return false;
  }

  const View2D *v2d = &region->v2d;

  r_geometry = src;
  auto project = [&](MutableSpan<float3> coords) {
    for (float3 &co : coords) {
      co = float3(
          ui::view2d_view_to_region_x(v2d, co.x), ui::view2d_view_to_region_y(v2d, co.y), 0.0f);
    }
  };
  project(r_geometry.positions_for_write());
  if (r_geometry.handle_positions_left().has_value()) {
    project(r_geometry.handle_positions_left_for_write());
  }
  if (r_geometry.handle_positions_right().has_value()) {
    project(r_geometry.handle_positions_right_for_write());
  }
  r_geometry.tag_positions_changed();
  return true;
}

/* \} */

}  // namespace ed::sculpt_paint
}  // namespace blender
