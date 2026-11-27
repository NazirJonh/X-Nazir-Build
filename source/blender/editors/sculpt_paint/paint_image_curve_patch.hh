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
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

#include "ED_paint.hh"

#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_live.hh"
#include "paint_curve_patch_session.hh"

#include <memory>
#include <optional>

struct ImagePool;

namespace blender {

struct Brush;
struct Material;
struct Paint;
struct PaintVector;
struct bContext;
struct Image;
struct MTex;
struct ReportList;
struct Scene;
struct SpaceImage;
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
 * - The canonical UV-space control curves the user edits (z = 0 everywhere; only x/y carry
 *   meaning), one per patch (#uv_curves).
 * - The patches built from those curves (#doc) plus their resolved texture bindings (#texture),
 *   rasterized directly by `paint_image_curve_patch_raster.hh` -- no dab-replay involved.
 * - Snapshot of every brush / paint parameter the writer reads directly
 *   (#ImageCurvePatchFrozenParams), so restamp is deterministic across UI mutations.
 * - Ownership of the single image undo transaction opened at the anchor stroke end. The handle
 *   pointer itself identifies the slot; closing through the owned-slot helpers from Stage 1
 *   verifies it before commit / abort.
 * - Selection / modal UI state consumed by the Stage 7 modal operator.
 */
struct ImageCurvePatchSession {
  /**
   * The reference tile of one patch: the UDIM tile whose pixel resolution turns that patch's UV
   * curve into the pixel-space geometry the rasterizer addresses. Per patch, because the curves
   * of a layer session may start on different tiles.
   */
  struct RefTile {
    int number = 0;
    float2 uv_origin = float2(0.0f, 0.0f);
    int2 resolution = int2(0, 0);
  };

  /**
   * Canonical UV curves, index-parallel to #doc.patches. Z component is always 0; positions and
   * handles carry UV only. The anchor path fills exactly one (see
   * #image_curve_patch_session_begin); a layer path fills one per container curve.
   */
  Vector<bke::CurvesGeometry> uv_curves;

  /** The canonical UV curve of the patch the modal editor acts on. Asserts a well-formed
   * document, like #CurvePatchDocument::active_item. */
  bke::CurvesGeometry &curve()
  {
    BLI_assert(this->doc.has_active_item());
    BLI_assert(this->uv_curves.index_range().contains(this->doc.active_patch));
    return this->uv_curves[this->doc.active_patch];
  }
  const bke::CurvesGeometry &curve() const
  {
    BLI_assert(this->doc.has_active_item());
    BLI_assert(this->uv_curves.index_range().contains(this->doc.active_patch));
    return this->uv_curves[this->doc.active_patch];
  }

  /* Frozen brush/paint fields the writer reads directly -- see the struct's own doc comment. */
  ImageCurvePatchFrozenParams params;

  /**
   * Randomize Color seed, frozen at session begin like the rest of #params. A patch re-stamps on
   * every live edit, so its randomized colors must come from a seed that does not change between
   * re-stamps -- a fresh seed per restamp would shift the patch's colors under the user's hands.
   * Empty when the brush had Randomize Color off at session begin.
   */
  std::optional<float3> initial_hsv_jitter;

  /* Per-patch fields with a home in `bke::CurvePatchParams` (radius, spacing, falloff, stamp
   * layout, ...), frozen at anchor time and overlaid with live brush state on every rebuild via
   * `curve_patch_params_live_overlay()` -- same split 3D Sculpt Mode uses. The anchor path has
   * exactly one; a layer path overlays each item's own frozen parameters (the container carries
   * per-curve values). */
  bke::CurvePatchParams frozen_patch_params;

  /* What the user edits, target-independent: every built patch, the resolved texture variants,
   * and the live-brush watchdog. Each item's `control_curve` lives in PIXELS of ITS reference
   * tile, not UV -- the canonical UV curve of the same patch is #uv_curves (see the
   * coordinate-space note on `paint_image_curve_patch_raster.hh`). Rebuilt by
   * `image_curve_patch_geometry_rebuild()` on every curve edit and every live-parameter change.
   *
   * The anchor path appends exactly one item; a layer path mirrors the row's container. The set
   * is FIXED for the session's life: the 2D modal adds and removes POINTS, never patches, so the
   * vectors parallel to #doc.patches cannot drift. */
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

  /* Per-patch reference tile: UDIM tile number and that tile's UV origin / pixel resolution at
   * the time of the last rebuild (spec §4.2, §9). Index-parallel to #doc.patches -- the anchor
   * path has exactly one entry, a layer path one per patch (curves may sit on different tiles).
   */
  Vector<RefTile> ref_tiles;

  /* Image-editor zoom at session open, frozen (spec §4.3) -- never recomputed, so zooming while
   * editing does not rescale an already-started patch. */
  float zoom_2d = 1.0f;
  /* Image pixels per brush-Size unit, frozen alongside the radius so the Size slider keeps
   * scaling a started patch (spec §4.3). A layer session leaves it 0: each container curve
   * carries its own `radius_per_size` in its frozen parameters, which the rebuild refreshes
   * instead. */
  float radius_per_size = 0.0f;

  /* The session's own capture of every image tile it has painted over, owned outright and freed
   * with the session. This is what a restore writes back, and at commit it becomes the one undo
   * history entry -- see #ED_image_paint_tile_map_new. Nothing is in flight on the global undo
   * stack while the session lives, so no other operator can take a transaction away from it. */
  PaintTileMap *tiles = nullptr;

  /* The image the session paints on -- recorded at begin time so the modal can notice the editor
   * being pointed at a different one. Pointer-only: the image is owned by `Main`.
   *
   * For a layer session this is the row's reference map (the Base Color map, or the first map
   * the row carries): it is what `resolve_reference_tile()` reads tile resolutions from, NOT a
   * statement that only this image is written -- in Material-canvas mode every enabled channel
   * map of the row is. */
  Image *image = nullptr;

  /* ----- Modal UI / selection state (read by the modal operator) ----- */
  /* True once `PAINT_OT_image_curve_patch_edit` has adopted this session in its `invoke()`. The
   * anchor stroke uses it to detect a session nobody took over and roll it back instead of
   * leaking it together with its open image-undo transaction. */
  bool modal_active = false;

  /* ----- Stack-layer session context (see `paint_curve_patch_layer.hh`) ----- */
  /**
   * True for a session entered from a #MA_PAINT_LAYER_SOURCE_CURVE_PATCH row
   * (#image_curve_patch_session_begin_layer) rather than from a Texture Paint anchor stroke.
   * The commit then writes the curves back into the row's container and requests the bake; a
   * cancel only restores the pixels. The brush the row's snapshot replaced is restored through
   * #layer_brush_restore on BOTH exits.
   */
  bool is_layer = false;
  /**
   * True when the layer session edits a Text Decal's bend curve rather than a Curve Patch effect
   * that lays stamps. The document still follows the container, but no pixel is ever written: the
   * restamp is skipped and the commit stores the curve alone, so the decal's coverage map -- which
   * the bent raster owns -- is never restored or painted over. See #curve_patch_layer_activate_2d.
   */
  bool is_bend_curve_only = false;
  /** True for a mask Curve Patch: no stamp texture, the Mask slot is coverage. */
  bool stamp_textures_disabled = false;
  /**
   * UV origin of the tile the Image Editor currently displays. The session's curves are in global
   * UV (so a canvas text or a 2D Curve Patch knows its UDIM tiles); the editor's region is one
   * tile, so the overlay and the cursor convert through this origin. Updated on every modal event
   * as the user changes the active tile.
   */
  float2 editor_uv_origin = {0.0f, 0.0f};
  /** The canonical curves as the bend session opened them: a cancel writes these back, because a
   * bend session writes the live curve into the container on every edit. Index-parallel to
   * #uv_curves. Empty for a session that is not a bend curve. */
  Vector<bke::CurvesGeometry> initial_uv_curves;
  /** The row this session edits, addressed by marker (never by raw pointer: the Outliner can
   * delete the row mid-session, and a dangling pointer on the commit path would crash). Null /
   * zero for an anchor session. */
  Material *layer_material = nullptr;
  bUUID layer_row_marker = {};
  /**
   * The previous-brush restore of a layer session: the single path that puts the user's own
   * brush back on every exit (commit, cancel, refused invoke). Null for an anchor session.
   * The type is only completed in the .cc, which owns the destructor call.
   */
  std::unique_ptr<class PrevBrushRestore> layer_brush_restore;

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
 * Open a LAYER session around a Stack Layers row's container (#MA_PAINT_LAYER_SOURCE_CURVE_PATCH
 * row, Space = 2D): adopt every curve item of \a vectors as a patch (no anchor stroke), freeze
 * each patch's parameters as they are stored in DNA, and target the row's channel maps the way
 * the 3D layer session does.
 *
 * \a ref_image is the row's reference map (the Base Color map, or the first map the row
 * carries): patch pixel space is built against its tile resolutions, and in Material-canvas mode
 * every enabled channel map of the row is written, not just this one. The Image Editor may
 * display a different image while the session runs -- Material-canvas mode tolerates that.
 *
 * The caller must have set the Material canvas (`PAINT_CANVAS_SOURCE_MATERIAL`), activated the
 * row's brush snapshot and ensured the row's channel maps before this, exactly like the 3D layer
 * entry does. \a ma / \a row_marker record the row for the commit write-back; the row is looked
 * up again BY MARKER at commit time, so a row deleted mid-session simply writes nothing back.
 *
 * Returns nullptr (and reports) when the container holds no usable curve, or when a session is
 * already active. The caller surfaces the report and rolls back its brush switch.
 */
ImageCurvePatchSession *image_curve_patch_session_begin_layer(bContext *C,
                                                              ReportList *reports,
                                                              PaintVector &vectors,
                                                              Brush *brush,
                                                              const Paint *paint,
                                                              Image *ref_image,
                                                              Material &ma,
                                                              const bUUID &row_marker,
                                                              bool stamp_textures_disabled = false);

/**
 * Whether a live 2D session edits the Curve Patch effect row identified by \a row_marker on
 * \a ma (see #ED_curve_patch_session_edits_row, which also consults the 3D sessions). False
 * without a live session, or for an anchor session.
 */
bool image_curve_patch_session_edits_row(const Material &ma, const bUUID &row_marker);

/**
 * Whether \a session shows and edits only its active curve. An anchor session is always solo
 * (it has exactly one patch); a layer session follows its row container's show-all flag
 * (#PAINT_VECTOR_SHOW_ALL_CURVES), and a row that went away mid-session degrades to solo rather
 * than to show-all -- the conservative side, since the curves it would reveal are unknown.
 * Read by the overlay, the pick and the modal's pickable-curve set.
 */
bool image_curve_patch_session_solo(const ImageCurvePatchSession &session);

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

/**
 * The global-UV origin of the UDIM tile the Image Editor displays: `(0,0)` for a single image, the
 * tile's corner otherwise. The session's curves are global UV; the editor's region is one tile.
 */
float2 image_curve_patch_editor_uv_origin(const SpaceImage *sima);

/* \} */

}  // namespace ed::sculpt_paint
}  // namespace blender
