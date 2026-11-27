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

#include <algorithm>

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_image.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_types.hh"
#include "BKE_paint_vector.hh"
#include "BKE_report.hh"
#include "BKE_undo_system.hh"

#include "BLI_index_range.hh"

#include "DNA_brush_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_enums.h"
#include "DNA_paint_vector_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_curve_patch.hh"
#include "ED_paint.hh"
#include "ED_paint_curve_draw.hh"

#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"
#include "WM_api.hh"


#include "UI_view2d.hh"

#include "mesh/paint_material_source.hh"
#include "paint_curve_patch_edit_intern.hh"
#include "paint_curve_patch_layer.hh"
#include "paint_image_curve_patch.hh"
#include "paint_image_curve_patch_raster.hh"
#include "selection/paint_image_select_intern.hh"

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

/**
 * Whether the session shows and edits only its active curve. An anchor session is always solo
 * (it has exactly one patch); a layer session follows its row container's show-all flag
 * (#PAINT_VECTOR_SHOW_ALL_CURVES), and a row that went away mid-session degrades to solo rather
 * than to show-all -- the conservative side, since the curves it would reveal are unknown.
 */
bool image_curve_patch_session_solo(const ImageCurvePatchSession &session)
{
  if (!session.is_layer || !curve_patch_layer_material_alive(session.layer_material) ||
      BLI_uuid_is_nil(session.layer_row_marker))
  {
    return true;
  }
  const MaterialPaintLayer *row = BKE_paint_layers_find(*session.layer_material,
                                                        session.layer_row_marker);
  if (row == nullptr || row->curve_patch == nullptr || row->curve_patch->vectors == nullptr) {
    return true;
  }
  return (row->curve_patch->vectors->flag & PAINT_VECTOR_SHOW_ALL_CURVES) == 0;
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

/**
 * Nested Curve Patch sessions are not allowed (one modal edit at a time per editor), so a session
 * that is still registered when a new one begins is disposed first. Nobody owns it anymore if its
 * modal is gone, and restoring its tiles puts the canvas back to a state the (dead) modal can no
 * longer contradict.
 */
static void image_curve_patch_session_discard_stale()
{
  if (g_active_session == nullptr) {
    return;
  }
  if (g_active_session->tiles != nullptr) {
    ED_image_paint_tile_map_restore(g_active_session->tiles);
  }
  MEM_delete(g_active_session);
  g_active_session = nullptr;
  curve_patch_point_texture_pick_reset();
}

/**
 * The fields both entries fill the same way: the frozen brush parameters, the color-jitter seed,
 * the session's own "before" pixels (nothing is pushed onto the undo stack until commit, so no
 * foreign operator can take a transaction out from under a patch that is still being edited), the
 * reference image and the view zoom.
 */
static void image_curve_patch_session_init_common(
    bContext *C, ImageCurvePatchSession &session, Brush *brush, const Paint *paint, Image *image)
{
  session.params = image_curve_patch_capture_params(brush, paint);
  /* Randomize Color: one seed per session, frozen like the rest of #params -- see the field's doc
   * comment on #ImageCurvePatchSession. */
  if (paint != nullptr && BKE_brush_color_jitter_get_settings(paint, brush)) {
    session.initial_hsv_jitter = seed_hsv_jitter();
  }
  session.tiles = ED_image_paint_tile_map_new();
  session.image = image;

  float zoomx = 1.0f, zoomy = 1.0f;
  get_imapaint_zoom(C, &zoomx, &zoomy);
  session.zoom_2d = std::max(zoomx, zoomy);
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
  image_curve_patch_session_discard_stale();
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->image == nullptr) {
    BKE_report(reports, RPT_ERROR, "Curve Patch requires an Image Editor with an open image");
    return nullptr;
  }

  /* Face selection masking: while enabled on any canvas object, the image's derived 2D selection
   * masks are rebuilt from the objects' face selections so the 2D patch respects the same faces
   * as the 3D viewport. */
  image_paint_selection_mask_from_face_selection(C, CTX_data_scene(C), sima->image);

  ImageUser iuser = {};
  iuser.framenr = sima->iuser.framenr;
  const ImBuf *ibuf = BKE_image_acquire_ibuf(sima->image, &iuser, nullptr);
  const bool has_four_channels = ibuf != nullptr && ibuf->channels == 4;
  BKE_image_release_ibuf(sima->image, const_cast<ImBuf *>(ibuf), nullptr);
  if (!has_four_channels) {
    BKE_report(reports, RPT_WARNING, "Image requires 4 color channels to paint");
    return nullptr;
  }

  /* One gate per session: a locked or hidden row takes no target, and the user hears why before
   * the patch starts rather than on every restamp. */
  if (Scene *scene = CTX_data_scene(C)) {
    Object *ob = CTX_data_active_object(C);
    if (ob != nullptr && scene->toolsettings != nullptr &&
        ed::sculpt_paint::material::paint_layers_stroke_gate_report(
            *C, *ob, scene->toolsettings->paint_mode, reports))
    {
      return nullptr;
    }
  }

  /* MEM_new, NOT `std::make_unique`: every path that ends this session frees it with #MEM_delete
   * (#image_curve_patch_session_commit, #image_curve_patch_session_cancel, and the nested-session
   * branch above), and handing a pointer from the global `operator new` to guardedalloc makes it
   * read whatever lies in front of the object as a `MemHead` -- the crash lands inside
   * `MEM_lockfree_freeN` on an address derived from that garbage. There is nothing for a
   * `unique_ptr` to guard here anyway: every failure exit is above this line, so the object cannot
   * leak between its construction and the assignment to `g_active_session` below. */
  ImageCurvePatchSession *session = MEM_new<ImageCurvePatchSession>(__func__);
  /* The anchor path has exactly one canonical curve and one patch; the vectors stay
   * index-parallel for the session's whole life (the 2D modal adds and removes POINTS, never
   * patches). */
  session->uv_curves.append(initial_curve);
  session->doc.patches.resize(1);
  session->ref_tiles.append_as();
  image_curve_patch_session_init_common(C, *session, brush, paint, sima->image);
  const float pixel_radius = BKE_brush_radius_get(paint, brush);
  session->radius_per_size = (pixel_radius / session->zoom_2d) /
                             float(std::max(1, BKE_brush_size_get(paint, brush)));

  g_active_session = session;
  return g_active_session;
}

ImageCurvePatchSession *image_curve_patch_session_begin_layer(bContext *C,
                                                              ReportList *reports,
                                                              PaintVector &vectors,
                                                              Brush *brush,
                                                              const Paint *paint,
                                                              Image *ref_image,
                                                              Material &ma,
                                                              const bUUID &row_marker)
{
  image_curve_patch_session_discard_stale();
  if (brush == nullptr || ref_image == nullptr) {
    BKE_report(reports, RPT_WARNING, "Curve Patch layer needs a brush snapshot and a channel map");
    return nullptr;
  }
  /* Unlike the anchor entry, the brush type is NOT constrained to a Draw image brush: the caller
   * validated the row's snapshot through `bke::brush::supports_curve_patch`, which is the gate
   * both spaces share. */

  /* The document is the persisted container: curves come in as UV (Space = 2D is what this entry
   * serves), parameters decoded per item. Curves too short to build are dropped the same way the
   * 3D layer entry drops them. */
  ImageCurvePatchSession *session = MEM_new<ImageCurvePatchSession>(__func__);
  curve_patch_document_from_vector(vectors, session->doc);
  session->doc.patches.remove_if(
      [](const CurvePatchItem &item) { return item.control_curve.points_num() < 2; });
  if (session->doc.patches.is_empty()) {
    BKE_report(
        CTX_wm_reports(C), RPT_WARNING, "Curve Patch layer has no usable control curve yet");
    MEM_delete(session);
    return nullptr;
  }
  session->doc.active_patch = std::clamp(session->doc.active_patch,
                                         0,
                                         std::max<int>(session->doc.patches.size() - 1, 0));

  /* The document items hold the canonical UV curves for now; move them into the session's
   * canonical vector and leave `control_curve` to the rebuild, which overwrites it with the
   * pixel-space copy each patch is rasterized from. */
  const int patches_num = session->doc.patches.size();
  session->uv_curves.reserve(patches_num);
  session->ref_tiles.resize(patches_num);
  for (const int i : IndexRange(patches_num)) {
    session->uv_curves.append(std::move(session->doc.patches[i].control_curve));
    session->doc.patches[i].control_curve = bke::CurvesGeometry();
  }

  image_curve_patch_session_init_common(C, *session, brush, paint, ref_image);
  session->is_layer = true;
  session->layer_material = &ma;
  session->layer_row_marker = row_marker;

  /* Face selection masking: while enabled on any canvas object, the reference map's derived 2D
   * selection masks are rebuilt from the objects' face selections, so the 2D patch respects the
   * same faces as the 3D viewport -- the same parity call the anchor path makes for the editor's
   * own image. Only the reference map is rebuilt (the anchor path masks exactly one image too);
   * the rasterizer gates every channel through ITS image's own mask state. */
  image_paint_selection_mask_from_face_selection(C, CTX_data_scene(C), ref_image);
  /* `radius_per_size` stays 0: each container curve carries its own frozen ratio, which
   * `image_curve_patch_params_resolve()` reads per item for a layer session. */

  g_active_session = session;
  return g_active_session;
}

bool image_curve_patch_session_edits_row(const Material &ma, const bUUID &row_marker)
{
  const ImageCurvePatchSession *session = g_active_session;
  return session != nullptr && session->is_layer && session->layer_material == &ma &&
         BLI_uuid_equal(session->layer_row_marker, row_marker);
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
  if (session->tiles != nullptr && !session->is_bend_curve_only) {
    /* Restore first, then re-stamp at final quality, and only then hand the captured tiles to the
     * undo system: the restore invalidates every tile, the final stamp re-validates exactly the
     * ones it touches, and the history entry ends up describing precisely what the canvas keeps.
     */
    curve_patch_restore_to_anchor(*session);
    image_curve_patch_raster_draw_final(C, *session);
    ED_image_undo_push_from_tile_map("Curve Patch", PaintMode::Texture2D, session->tiles);
  }
  if (session->is_layer && C != nullptr) {
    /* Write the curves back into the row's container, request the bake and push the memfile
     * step, exactly like the 3D layer session's commit does. The session is still alive here, so
     * the memfile snapshot captures the pre-restore brush; the destructor below switches the
     * user's own brush back either way. */
    curve_patch_layer_commit_session_2d(*C, *session);
  }
  g_active_session = nullptr;
  curve_patch_point_texture_pick_reset();
  /* #layer_brush_restore (a layer session) puts the user's previous brush back through its
   * destructor; an anchor session has none. */
  MEM_delete(session);
}

void image_curve_patch_session_cancel(bContext *C, ImageCurvePatchSession *session)
{
  BLI_assert(session == g_active_session);
  if (session == nullptr) {
    return;
  }
  /* Put the pristine pixels back and write no history entry at all: nothing was ever pushed onto
   * the undo stack for this session, so there is nothing to discard either. A layer session
   * writes nothing back either way -- the container keeps the curves it had before the session
   * (see #curve_patch_layer_cancel_session_2d, which also notifies the stack). */
  if (!session->is_bend_curve_only) {
    curve_patch_restore_to_anchor(*session);
  }
  if (session->is_layer) {
    if (session->is_bend_curve_only && C != nullptr) {
      /* A bend session writes its container on every edit, so the cancel has to put the curves it
       * opened with back. */
      curve_patch_layer_restore_session_2d(*C, *session);
    }
    curve_patch_layer_cancel_session_2d(*session);
  }
  g_active_session = nullptr;
  curve_patch_point_texture_pick_reset();
  /* #layer_brush_restore (a layer session) puts the user's previous brush back through its
   * destructor; an anchor session has none. */
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
  /* The watchdog compares the ACTIVE patch's resolved parameters (the patch the modal acts on):
   * the live inputs it watches are session-wide, but the parameters a rebuild produces are
   * per-item, and only the active one is what an edit flow reads back. */
  r_params = image_curve_patch_params_resolve(session, paint, brush, session.doc.active_item());
  r_params.final_quality = false;

  CurvePatchLiveInputs in = curve_patch_live_inputs_capture(paint, brush, OB_MODE_TEXTURE_PAINT);
  in.blend = brush.blend;
  /* Toggling Symmetry X/Y mid-session does not touch the Brush at all -- #symmetry_flags lives on
   * `Paint` -- so without this the watchdog's compare never changed and the checkbox had no effect
   * until some UNRELATED live input (color, alpha, ...) happened to re-stamp anyway. */
  in.image_symmetry_flags = int(paint.symmetry_flags & (PAINT_SYMM_X | PAINT_SYMM_Y));
  return in;
}

bool image_curve_patch_session_sync_live_brush(bContext *C, ImageCurvePatchSession *session)
{
  /* A curve-only session edits a container, never a canvas: no live brush reaches pixels, so
   * there is nothing to re-stamp. */
  if (session != nullptr && session->is_bend_curve_only) {
    return false;
  }
  /* The watchdog reads the ACTIVE patch's parameters: a session whose active patch is gone (its
   * container emptied or re-ordered under it) has nothing to compare, and reading it would
   * dereference past the patch vector. */
  if (session == nullptr || !session->doc.has_active_item()) {
    return false;
  }
  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush *brush = paint ? BKE_paint_brush_for_read(paint) : nullptr;
  if (brush == nullptr) {
    return false;
  }

  /* A panel edit of the row's curves (Width, Opacity, Hide, Mirror, the active item) lands in the
   * container; the live document follows it, and the restamp below shows it. */
  if (session->is_layer && curve_patch_layer_follow_2d(*session)) {
    image_curve_patch_session_restore_and_restamp(C, session);
    return true;
  }

  bke::CurvePatchParams live_params;
  const CurvePatchLiveInputs live = image_curve_patch_live_inputs_capture(
      *session, *paint, *brush, live_params);
  /* The per-point texture browse writes its attribute from an RNA setter with no context (see
   * #CurvePatchDocument::pending_point_texture_restamp); that write is invisible to the live-input
   * digest, so the flag forces the re-stamp. */
  const bool pending_texture_restamp = session->doc.pending_point_texture_restamp;
  if (!pending_texture_restamp && live == session->doc.last_synced &&
      live_params == session->doc.last_synced_params)
  {
    return false;
  }
  session->doc.pending_point_texture_restamp = false;

  const bool pool_rebuild = live.needs_texture_pool_rebuild(session->doc.last_synced);
  if (pool_rebuild) {
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
  /* The bend curve is a container: there are no pixels to restore, rebuild or stamp. Write the
   * live curve back and refresh the decal so the text follows the drag. */
  if (session->is_bend_curve_only) {
    curve_patch_layer_live_update_2d(*C, *session);
    return;
  }
  curve_patch_restore_to_anchor(*session);
  image_curve_patch_geometry_rebuild(C, *session);
  image_curve_patch_raster_draw(C, *session);

  /* Record the brush state this stamp was built from. Every path that puts pixels on the canvas
   * goes through here, so this is the one place the live-brush watchdog's baseline can be kept
   * honest -- seeding it anywhere else would leave the first poll comparing against defaults and
   * re-stamping for no reason. */
  if (!session->doc.has_active_item()) {
    return;
  }
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

float2 image_curve_patch_editor_uv_origin(const SpaceImage *sima)
{
  if (sima == nullptr) {
    return float2(0.0f);
  }
  /* Legacy tile numbers (0, 1) and a non-tiled image both answer the UV origin. */
  return BKE_image_get_tile_uv_origin(sima->iuser.tile);
}

bool ED_image_curve_patch_overlay_geometry_get(const ARegion *region,
                                               bke::CurvesGeometry &r_geometry)
{
  const ImageCurvePatchSession *session = g_active_session;
  if (session == nullptr || region == nullptr || !session->doc.has_active_item()) {
    return false;
  }
  const bke::CurvesGeometry &src = session->curve();
  if (src.points_num() == 0 || src.curves_num() == 0) {
    return false;
  }

  const View2D *v2d = &region->v2d;

  r_geometry = src;
  auto project = [&](MutableSpan<float3> coords) {
    for (float3 &co : coords) {
      float region_x, region_y;
      /* #view2d_view_to_region_fl, not the un-rotated per-axis #view2d_view_to_region_x/y: those
       * two skip #View2D::rotation entirely (they exist for callers that genuinely want the
       * axis-aligned rect, e.g. scrollers), so the overlay's points stayed screen-locked instead
       * of turning with the canvas once #SpaceImage::rotation was non-zero. */
      ui::view2d_view_to_region_fl(v2d,
                                   co.x - session->editor_uv_origin.x,
                                   co.y - session->editor_uv_origin.y,
                                   &region_x,
                                   &region_y);
      co = float3(region_x, region_y, 0.0f);
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

/** Project \a r_geometry (canvas UV) into region pixels, rotation included. */
static void image_curve_patch_project_to_region(const View2D &v2d,
                                                const float2 uv_origin,
                                                bke::CurvesGeometry &r_geometry)
{
  const auto project_coords = [&](MutableSpan<float3> coords) {
    for (float3 &co : coords) {
      float region_x, region_y;
      ui::view2d_view_to_region_fl(
          &v2d, co.x - uv_origin.x, co.y - uv_origin.y, &region_x, &region_y);
      co = float3(region_x, region_y, 0.0f);
    }
  };
  project_coords(r_geometry.positions_for_write());
  if (r_geometry.handle_positions_left().has_value()) {
    project_coords(r_geometry.handle_positions_left_for_write());
  }
  if (r_geometry.handle_positions_right().has_value()) {
    project_coords(r_geometry.handle_positions_right_for_write());
  }
  r_geometry.tag_positions_changed();
}

/**
 * The passive preview of the active 2D Space Curve Patch effect of \a ob: its stored UV curves,
 * with no session behind them. The row must hold its curves in UV already (#stored_space), or the
 * coordinates would be object-space positions read as UV.
 */
static bool image_curve_patch_passive_overlay_get(const ARegion *region,
                                                  const Object *ob,
                                                  Vector<bke::CurvesGeometry> &r_projected,
                                                  CurvePatchOverlayData &r_data)
{
  if (region == nullptr || ob == nullptr) {
    return false;
  }
  const Material *ma = BKE_object_material_get(const_cast<Object *>(ob), ob->actcol);
  if (ma == nullptr) {
    return false;
  }
  const MaterialPaintLayer *row = BKE_paint_layers_active_layer_get(const_cast<Material &>(*ma));
  if (row == nullptr || row->source != MA_PAINT_LAYER_SOURCE_CURVE_PATCH ||
      row->curve_patch == nullptr || row->curve_patch->vectors == nullptr ||
      (row->flag & MA_PAINT_LAYER_ENABLED) == 0 ||
      curve_patch_layer_row_space(*row) != MA_CURVE_PATCH_SPACE_2D ||
      row->curve_patch->stored_space != MA_CURVE_PATCH_SPACE_2D)
  {
    return false;
  }
  const PaintVector &pv = *row->curve_patch->vectors;
  const bool solo = (pv.flag & PAINT_VECTOR_SHOW_ALL_CURVES) == 0;
  /* `r_data.splines` points into `r_projected`: reserve once so no append reallocates. */
  r_projected.reserve(pv.items_num);
  int active_curve = 0;
  for (const int i : IndexRange(pv.items_num)) {
    const PaintVectorItem &item = pv.items[i];
    if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH || (solo && i != pv.active_item) ||
        !BKE_paint_vector_item_curve_geometry_is_valid(item))
    {
      continue;
    }
    const bke::CurvesGeometry &src = item.curve_patch_geometry.wrap();
    if (src.points_num() == 0 || src.curves_num() == 0) {
      continue;
    }
    if (i == pv.active_item) {
      active_curve = r_data.splines.size();
    }
    r_projected.append(src);
    image_curve_patch_project_to_region(region->v2d, float2(0.0f), r_projected.last());
    r_data.splines.append(&r_projected.last());
  }
  if (!r_data.splines.is_empty()) {
    r_data.active_index = active_curve;
  }
  return !r_data.splines.is_empty();
}

bool ED_image_curve_patch_overlay_data_get(const ARegion *region,
                                           Vector<bke::CurvesGeometry> &r_projected,
                                           CurvePatchOverlayData &r_data,
                                           const Object *ob)
{
  r_projected.clear();
  r_data.splines.clear();
  r_data.active_index = -1;
  const ImageCurvePatchSession *session = g_active_session;
  if (session == nullptr) {
    return image_curve_patch_passive_overlay_get(region, ob, r_projected, r_data);
  }
  if (region == nullptr || !session->doc.has_active_item()) {
    return false;
  }

  /* Solo: the curves that are not selected are neither drawn nor pickable -- the same contract
   * the 3D session's overlay honors. An anchor session is always solo (it has one patch); a
   * layer session follows its row container's show-all flag. */
  const int active_patch = session->doc.active_patch;
  const bool solo = image_curve_patch_session_solo(*session);

  /* Reserve ONCE up front: `r_data.splines` holds pointers INTO `r_projected`, so a mid-loop
   * reallocation would dangle every pointer appended before it. The loop appends at most
   * `uv_curves.size()` entries, so one reserve covers them all. */
  r_projected.reserve(session->uv_curves.size());

  for (const int i : session->uv_curves.index_range()) {
    const bke::CurvesGeometry &src = session->uv_curves[i];
    if (src.points_num() == 0 || src.curves_num() == 0) {
      continue;
    }
    if (solo && i != active_patch) {
      continue;
    }
    r_projected.append(src);
    image_curve_patch_project_to_region(region->v2d, session->editor_uv_origin, r_projected.last());
    r_data.splines.append(&r_projected.last());
  }
  /* The active index points into the FILLED list: under solo the active curve is the only entry,
   * so it is 0 there, exactly like the 3D session's solo branch. */
  if (!r_data.splines.is_empty()) {
    r_data.active_index = solo ? 0 : active_patch;
  }
  return !r_data.splines.is_empty();
}

/* \} */

}  // namespace ed::sculpt_paint
}  // namespace blender
