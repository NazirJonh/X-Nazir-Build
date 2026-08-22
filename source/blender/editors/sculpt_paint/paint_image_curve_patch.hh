/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Live 2D Curve Patch session for the Image Editor (SPACE_IMAGE, SI_MODE_PAINT,
 * PaintMode::Texture2D).
 *
 * The session owns the canonical UV-space edit curve (#bke::CurvesGeometry) for one in-progress
 * patch, plus everything that must survive across restore-and-re-stamp cycles without rewriting
 * pixels more than once. It is intentionally NOT a DNA / RNA data-block -- it is editor runtime
 * state, the same way the 3D Sculpt Curve Patch session lives on `SculptSession`. Modal hit
 * testing and overlay drawing (Stage 7) call back into
 * #image_curve_patch_session_active_get() to discover the live session.
 *
 * Lifetime: bound to the editor's brush modal handler. Born at the end of the anchor stroke
 * (Stage 6), adopted by the modal operator (Stage 7) for editing, and destroyed either on Enter
 * (commit) or on Esc / wrong context (cancel).
 */

#pragma once

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

#include "ED_paint.hh"

#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_live.hh"
#include "paint_curve_patch_session.hh"

#include <memory>

struct ImagePool;

namespace blender {

struct Brush;
struct Paint;
struct bContext;
struct Image;
struct MTex;
struct ReportList;
struct Scene;
struct View2D;

namespace ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Frozen Parameters
 * \{ */

/**
 * The brush/paint fields the writer reads directly (spec §5.2, "Live, not in params" per the 3D
 * contract documented on `curve_patch_params_live_overlay()`). Everything with a home in
 * `bke::CurvePatchParams` lives in `ImageCurvePatchSession::frozen_patch_params` instead --
 * duplicating those fields here would create a second source of truth.
 */
struct ImageCurvePatchFrozenParams {
  /* Color the anchor stroke used, in scene-linear. The writer premultiplies/converts this per
   * buffer type at blend time (spec §7.5); it is never written to the image directly here. */
  float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  float alpha = 0.0f;   /* `BKE_brush_alpha_get` at anchor time. */
  short blend = 0;      /* `brush->blend` (`IMB_BlendMode`). */
  short brush_type = 0; /* IMAGE_PAINT_BRUSH_TYPE_DRAW (only valid type). */
};

/* \} */

/* -------------------------------------------------------------------- */
/** \name Session
 * \{ */

/**
 * One in-progress 2D Curve Patch. Holds:
 * - The canonical UV-space #bke::CurvesGeometry the user is editing (z = 0 everywhere; only x/y
 *   carry meaning).
 * - The patch built from that curve (#patch) plus its resolved texture bindings (#texture),
 *   rasterized directly by `paint_image_curve_patch_raster.hh` -- no dab-replay involved.
 * - Snapshot of every brush / paint parameter the writer reads directly
 *   (#ImageCurvePatchFrozenParams), so restamp is deterministic across UI mutations.
 * - Ownership of the single image undo transaction opened at the anchor stroke end. The handle
 *   pointer itself identifies the slot; closing through the owned-slot helpers from Stage 1
 *   verifies it before commit / abort.
 * - Selection / modal UI state consumed by the Stage 7 modal operator.
 */
struct ImageCurvePatchSession {
  /* Canonical UV curve. Z component is always 0; positions and handles carry UV only. */
  bke::CurvesGeometry curve;

  /* Frozen brush/paint fields the writer reads directly -- see the struct's own doc comment. */
  ImageCurvePatchFrozenParams params;

  /* Per-patch fields with a home in `bke::CurvePatchParams` (radius, spacing, falloff, stamp
   * layout, ...), frozen at anchor time and overlaid with live brush state on every rebuild via
   * `curve_patch_params_live_overlay()` -- same split 3D Sculpt Mode uses. */
  bke::CurvePatchParams frozen_patch_params;

  /* What the user edits, target-independent: the single built patch (`doc.active_item()`), its
   * resolved texture variants, and the live-brush watchdog. The item's `control_curve` lives in
   * PIXELS of the reference tile, not UV -- see #ref_tile_number and the coordinate-space note on
   * `paint_image_curve_patch_raster.hh`. Rebuilt by `image_curve_patch_geometry_rebuild()` on
   * every curve edit and every live-parameter change.
   *
   * Exactly one item, appended by #image_curve_patch_session_begin: the canvas has no multi-patch
   * path yet, so nothing here may assume more than one. */
  CurvePatchDocument doc;

  /* Texture pool for thread-safe sampling. Lazily created -- see #tex_pool_ensure().
   *
   * Stays on the session rather than on #doc: the 3D counterpart is `SculptSession`'s own pool,
   * shared with every other sculpt brush, so a pool on the document would be a second one there.
   * Which pool a build samples through is a property of the owner, not of the patch. */
  ImagePool *tex_pool = nullptr;

  /**
   * Create `tex_pool` on first use, return the existing one otherwise. Mirrors
   * `SculptSession::tex_pool_ensure()`. The only legal way to obtain the pool -- nothing reads
   * `tex_pool` directly except this method and the session's own teardown. Must be called before
   * any `threading::parallel_for` that will use the pool, never from inside one (races the lazy
   * creation across threads).
   */
  ImagePool &tex_pool_ensure();

  /**
   * Drop the pool so the next #tex_pool_ensure() builds a fresh one. Called only when the SET of
   * sampled images or their mapping changed -- see
   * #CurvePatchLiveInputs::needs_texture_pool_rebuild().
   */
  void tex_pool_invalidate();

  /* UDIM tile number the patch is built in, and that tile's UV origin / pixel resolution at the
   * time of the last rebuild (spec §4.2, §9). */
  int ref_tile_number = 0;
  float2 ref_tile_uv_origin = float2(0.0f, 0.0f);
  int2 ref_tile_resolution = int2(0, 0);

  /* Image-editor zoom at session open, frozen (spec §4.3) -- never recomputed, so zooming while
   * editing does not rescale an already-started patch. */
  float zoom_2d = 1.0f;
  /* Image pixels per brush-Size unit, frozen alongside the radius so the Size slider keeps
   * scaling a started patch (spec §4.3). */
  float radius_per_size = 0.0f;

  /* The session's own capture of every image tile it has painted over, owned outright and freed
   * with the session. This is what a restore writes back, and at commit it becomes the one undo
   * history entry -- see #ED_image_paint_tile_map_new. Nothing is in flight on the global undo
   * stack while the session lives, so no other operator can take a transaction away from it. */
  PaintTileMap *tiles = nullptr;

  /* The image the session paints on -- recorded at begin time so the modal can notice the editor
   * being pointed at a different one. Pointer-only: the image is owned by `Main`. */
  Image *image = nullptr;

  /* ----- Modal UI / selection state (read by the modal operator) ----- */
  /* True once `PAINT_OT_image_curve_patch_edit` has adopted this session in its `invoke()`. The
   * anchor stroke uses it to detect a session nobody took over and roll it back instead of
   * leaking it together with its open image-undo transaction. */
  bool modal_active = false;

  ~ImageCurvePatchSession();
};

/* \} */

/* -------------------------------------------------------------------- */
/** \name Lifecycle
 * \{ */

/**
 * Open a session around the canonical initial UV-space curve. The caller passes the locally
 * evaluated initial path on the way out of the anchor stroke. The session:
 * - Snapshots all frozen paint parameters from `brush` and `paint`.
 * - Opens a fresh image undo transaction owned by the session (only when the slot check above
 *   succeeds).
 * - Rejects an image whose active `ImBuf` does not have 4 channels, with the same report text
 *   `paint_2d_new_stroke()` uses (spec §11).
 *
 * Returns nullptr (and reports) when the slot is unavailable or the brush is not a baseline
 * `IMAGE_PAINT_BRUSH_TYPE_DRAW` -- both are explicit "do not start a session" signals. The
 * caller is expected to surface the report and abort the modal takeover.
 */
ImageCurvePatchSession *image_curve_patch_session_begin(bContext *C,
                                                        ReportList *reports,
                                                        const bke::CurvesGeometry &initial_curve,
                                                        Brush *brush,
                                                        const Paint *paint);

/**
 * Commit: re-stamp at final quality, turn the session's captured tiles into a single undo history
 * entry, free the active paint stroke handle, drop the session.
 */
void image_curve_patch_session_commit(bContext *C, ImageCurvePatchSession *session);

/**
 * Cancel: restore the canvas to its pre-anchor state from the session's own tiles, write no
 * history entry at all, free the active paint stroke handle, drop the session.
 */
void image_curve_patch_session_cancel(bContext *C, ImageCurvePatchSession *session);

/* \} */

/* -------------------------------------------------------------------- */
/** \name Active Session Lookup (Stage 7 modal operator's poll)
 * \{ */

/** True when a session is currently bound (anchor finished, modal not yet disposed). */
bool image_curve_patch_session_active();

/** Return the active session or nullptr. The pointer is non-owning -- call commit / cancel to
 * dispose. */
ImageCurvePatchSession *image_curve_patch_session_active_get();

/* \} */

/* -------------------------------------------------------------------- */
/** \name Restore & Re-stamp
 * \{ */

/**
 * Single modify entry point used by Stage 7 modal handlers: restore tiles to the anchor state,
 * rebuild the patch geometry from the (possibly just-edited) canonical curve, and rasterize it
 * directly onto the canvas (spec §7, §9).
 */
void image_curve_patch_session_restore_and_restamp(bContext *C, ImageCurvePatchSession *session);

/**
 * Notice a brush edit made from the UI while the modal edit is running (the Stroke panel's Curve
 * Patch settings, the texture slots, color / strength, ...) and re-stamp the canvas.
 *
 * Nothing pushes those edits at the session -- RNA only sends `NC_BRUSH | NA_EDITED` -- so the
 * modal polls instead, exactly like 3D Sculpt Mode's `curve_patch_edit_modal()`. Returns true when
 * a change was found and a re-stamp was issued.
 */
bool image_curve_patch_session_sync_live_brush(bContext *C, ImageCurvePatchSession *session);

/* \} */

/* -------------------------------------------------------------------- */
/** \name Screen Adapter (read by Stage 7 overlay / hit-test code)
 * \{ */

/**
 * Take a region-local pixel position (`int[2]` matching #wmEvent::mval -- NOT the
 * window-relative #wmEvent::xy) and convert it to canonical UV coordinates through the active
 * #View2D of the image's current `wmRegion`.
 *
 * The inverse direction is #ED_image_curve_patch_overlay_geometry_get(), which projects the
 * whole canonical curve into region pixels for the overlay.
 */
void image_curve_patch_region_to_uv(bContext *C, const int region_mval[2], float r_uv[2]);

/* \} */

}  // namespace ed::sculpt_paint
}  // namespace blender
