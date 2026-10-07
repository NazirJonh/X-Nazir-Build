/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Compositor of the Image Editor shape drawing tools: rasterizes shapes into coverage and
 * writes it into the paint targets (the Canvas image, or the PBR paint channel images), with
 * one image undo step per bake.
 *
 * Every bake is self-contained: symmetry copies are derived here, the affected regions are the
 * union over all shapes (the copies included), fill is composited before stroke, profiles and
 * the selection mask are applied per pixel. Rasterization runs in row strips to keep peak
 * memory low on large tiles.
 *
 * The Pixel operators resolve their targets from the context (#shape_bake); the Vector session
 * passes its own stored targets (#shape_composite_into), so a session bakes into the editor
 * that owns it no matter which context drives the commit.
 *
 * The same header also declares the canvas-space symmetry and UDIM tile coordinate helpers the
 * compositor builds on (Image Editor only; the pure geometry lives in `paint_shape.hh`), and the
 * Image Editor Vector session lifetime with its 2D #VectorEditHost target (the gestures live in
 * #ShapeVectorEditor).
 */

#pragma once

#include <cstdint>
#include <memory>

#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_image_types.h"

#include "../shapes/paint_shape_target.hh"
#include "../shapes/paint_vector_editor.hh"
#include "../selection/paint_image_select_floating.hh"

namespace blender {

struct ARegion;
struct bContext;
struct Image;
struct ImageUser;
struct ImBuf;
struct Main;
struct Object;
struct Paint;
struct PaintMaterialImageTarget;
struct SpaceImage;
struct ToolSettings;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

/** Minimum interval between two live-preview shading tags, shared by the 2D Image Editor session
 * (#ImageTilesBackend) and the 3D Image-canvas session (#SculptImageBackend). The tile write it
 * follows is never throttled; only the redraw tag is. */
constexpr double SHAPE_PREVIEW_SHADING_TAG_INTERVAL = 0.1;

/**
 * True when the brush-derived preview fields of \a a and \a b are equal: the stroke/fill opacity
 * and blend (#style_brush_values_from_brush) and the per-channel values (#style_channels_from_brush).
 * A brush / settings notifier can then skip the re-composite: a changed brush size (not part of the
 * style) or an unrelated setting no longer re-bakes the live preview.
 */
bool shape_brush_style_equal(const ShapeStyle &a, const ShapeStyle &b);

/** What a target map holds: a painted channel (color/scalar/normal), or a layer mask coverage
 * Only #Channel exists in this tree; #Mask arrives with the layers merge and
 * is switched on in the adapter #shape_target_from_material_target. */
enum class ShapeTargetKind : int8_t {
  Channel,
  Mask,
};

/** One image a shape bakes into: the canvas image (#channel < 0) or a PBR channel image. */
struct ShapeTarget {
  Image *image = nullptr;
  /** Owner's ImageUser; may be null (a local default is used for the tile lookups). */
  ImageUser *iuser = nullptr;
  /** #eMaterialPaintChannel when >= 0, -1 for the plain canvas. */
  int channel = -1;
  /** Channel data or layer mask. */
  ShapeTargetKind kind = ShapeTargetKind::Channel;
  /** Name of the UV map the target projects through; empty means the object's active UV.
   * Always empty until the Stack Layers merge fills it through the adapter. */
  StringRef uv_map_name;
  /** `Image::id.session_uid` captured at resolution, so a write can tell whether the Image is
   * still the one the session resolved. */
  uint32_t image_session_uid = 0;
};

/**
 * Adapt one resolver target to a shape target. The single place the mask / UV fields of
 * a #PaintMaterialImageTarget are mapped onto #ShapeTarget; today always a channel with an empty
 * UV, matching the current resolution.
 */
ShapeTarget shape_target_from_material_target(const PaintMaterialImageTarget &target);

/**
 * Create the missing channel maps of \a ob's material for the current context's PBR targets.
 * Run once when a draw starts (the Pixel operator's invoke, the Vector session's begin), before
 * #composite_targets_get resolves them: the write paths themselves must never allocate images,
 * since they run outside any undo step that would record the creation.
 *
 * \param paint: the channel source for this context -- `tool_settings.imapaint.paint` in the
 * Image Editor, `tool_settings.sculpt.paint` in Sculpt Mode; the two modes keep independent
 * visible-channel sets and brush.
 * \param settings: decides the channel set (Override Channels, per-part `use`): the live session's
 * own copy for a session, the shared tool settings block for a Pixel draw.
 */
void composite_targets_ensure_writable(bContext *C,
                                       Object *ob,
                                       Paint &paint,
                                       const PaintShapeSettings &settings);

/**
 * Images the shape should bake into for the current context: the Image Editor's image when
 * invoked there, otherwise the paint-mode canvas of \a ob.
 *
 * \param paint: the channel source for this context (see #composite_targets_ensure_writable).
 *
 * \note The channel set is resolved by #BKE_paint_shape_target_channels, the same rule the 3D
 * attribute backend and the tool UI (`PaintShapeSettings.target_channels`) use.
 */
Vector<ShapeTarget> composite_targets_get(bContext *C,
                                          Object *ob,
                                          Paint &paint,
                                          const PaintShapeSettings &settings);

/**
 * Human-readable reason the current context's Material canvas resolved no writable target,
 * or null when targets are not the problem (not a Material canvas, or a target is available).
 *
 * The shape tools call this to report a refusal instead of silently drawing somewhere else:
 * in Material mode #composite_targets_get never falls back to the Image Editor image, so an empty
 * result here means the bake must not run. When Stack Layers merge this returns their specific
 * refusal (frozen bake target, missing UV map, active row is a folder, ...); today it is the
 * generic "no writable material channel map" text.
 */
const char *shape_targets_refusal_message(bContext *C, Object *ob);

/**
 * Fill \a style's PBR channel values from the active image-paint brush (the default source:
 * brush values, with per-part overrides only under #PAINT_SHAPE_CHANNELS_OVERRIDE). No-op in
 * override mode or without a material-paint brush: the shape's own channel values stand.
 * Both write paths apply it - the Pixel bake right before compositing, the Vector session on
 * every style refresh - so the preview shows exactly what the commit will write.
 *
 * \param paint: the channel source for this context (see #composite_targets_ensure_writable).
 */
void style_channels_from_brush(Paint &paint, ShapeStyle &style);

/** Set the overall opacity and canvas blend from the active brush: the brush/unified Strength as
 * both stroke and fill opacity, and the brush's blend mode for both parts. The shape colors are
 * NOT touched here: they come from #PaintShapeSettings (#style_from_settings). PBR channels keep
 * their own per-channel blend. */
void style_brush_values_from_brush(Paint &paint, ShapeStyle &style);

/**
 * Composite \a shapes with \a style into the explicit \a targets.
 *
 * The shapes are already symmetry-expanded, so callers pass the output of #shape_bake or the
 * Vector session; their pixels all refer to the session tile. Tile regions are the
 * union over every expanded shape.
 *
 * The shapes are defined in reference-tile pixels and each target tile rasterizes them at its
 * own resolution: a tile of a different size than #CanvasTile::ref_tile_size gets the geometry
 * and the pixel-based style widths scaled (#shapes_scale_to_target), so a channel map of half
 * the canvas resolution receives the same UV-space shape.
 *
 * With \a push_undo the write collapses into one labeled image undo step, the images are tagged
 * for the depsgraph and \a ob's material shading refreshes; without it (the Vector session's
 * live preview) only the touched regions are marked and the notifier stays light.
 *
 * \return true when at least one tile was written.
 */
bool shape_composite_into(Span<ShapeTarget> targets,
                          Span<PaintShape> shapes,
                          const CanvasTile &tile,
                          const ShapeStyle &style,
                          bool push_undo,
                          const char *undo_name,
                          Object *ob);

/**
 * Bake \a shapes with \a style into the current context targets (#composite_targets_get).
 * The Pixel path; the Vector session uses #shape_composite_into with its own targets.
 */
bool shape_bake(bContext *C,
                Span<PaintShape> shapes,
                const CanvasTile &tile,
                const ShapeStyle &style,
                bool push_undo,
                const char *undo_name);

/** Full-tile snapshot of one target tile (the Vector session's preview restore source). */
ImBuf *shape_tile_backup_init(Image *image, const ImageUser *owner_iuser, int tile_number);

/** Restore a full-tile snapshot and mark the image for redraw. */
void shape_tile_backup_restore(Image *image,
                               const ImageUser *owner_iuser,
                               int tile_number,
                               const ImBuf &backup);

/** Restore only the \a region of a full-tile snapshot. \a region is in tile pixels, half-open
 * and clipped to the tile. */
void shape_tile_backup_restore_region(Image *image,
                                      const ImageUser *owner_iuser,
                                      int tile_number,
                                      const ImBuf &backup,
                                      const rcti &region);

/** Expand \a r_region to also cover \a other (both half-open tile rects, either may be empty). */
void shape_region_union(rcti &r_region, const rcti &other);
/**
 * Image-tile write target of the Vector session: resolves the PBR channel maps / editor image
 * once, keeps the per-tile full backups the live preview restores from, and composites through
 * #shape_composite_into. Owning the storage here is what lets the session avoid ImBuf entirely.
 *
 * The shapes and style passed to #preview / #commit must already be symmetry-expanded and carry
 * the final channel values (see #ShapeTargetBackend).
 */
class ImageTilesBackend : public ShapeTargetBackend {
 public:
  /** \a settings is read at #begin only and must outlive the call; a session passes its own copy.
   * \a tile is the reference tile the session shapes refer to. */
  ImageTilesBackend(const PaintShapeSettings &settings, const CanvasTile &tile);
  ~ImageTilesBackend() override;

  bool begin(bContext &C, ReportList *reports) override;
  bool preview(bContext *C,
               ReportList *reports,
               Span<PaintShape> shapes,
               const ShapeStyle &style) override;
  bool commit(bContext *C,
              ReportList *reports,
              Span<PaintShape> shapes,
              const ShapeStyle &style,
              const char *undo_name) override;
  void cancel() override;

  /** Whether every resolved target Image is still the one the session resolved at #begin.
   * A dead target means an undo / material edit replaced it, and the session must not write. */
  bool targets_alive() const override;

 private:
  /** Full-tile backup of one affected tile, with the region last previewed on it. */
  struct TileBackup {
    /** Index into #targets_ the backed-up tile belongs to (tile numbers repeat across targets). */
    int target_index = 0;
    int tile_number = 0;
    ImBuf *orig = nullptr;
    /** Last region the preview painted on this tile; empty before the first preview write. */
    rcti dirty = {0, 0, 0, 0};
  };

  TileBackup *backups_find(int target_index, int tile_number);
  /** Restore every backup tile fully (before a commit). */
  void restore_all();

  /**
   * Whether \a target's resolved Image still exists, looked up by `session_uid` so the stored
   * pointer is never dereferenced (it is dangling once the Image is freed). Null \a bmain_ (a
   * test backend) counts every target as alive.
   */
  bool target_alive(const ShapeTarget &target) const;

  /** Settings the channel set is resolved from at #begin. */
  const PaintShapeSettings &settings_;
  /** Reference tile of the session shapes; fixed for the session. */
  CanvasTile tile_;
  Vector<ShapeTarget> targets_;
  /** Owned ImageUsers: the material-node ones can be freed mid-session. See the session begin. */
  Vector<ImageUser> target_iusers_;
  Vector<TileBackup> backups_;
  /** Material-map shading tag target: the active object when #begin resolved the targets. */
  Object *ob_ = nullptr;
  /** `Main` the targets were resolved in; the liveness lookup goes through it. */
  Main *bmain_ = nullptr;
  /** Time of the last throttled shading tag of the PBR preview. */
  double last_shading_tag_ = 0.0;
};

/* -------------------------------------------------------------------- */
/** \name Canvas symmetry
 * \{ */

/** \a shape followed by its canvas-space symmetry copies (when enabled and affecting painting).
 * Copies are spline-based: a non-affine symmetry (circle inversion) densifies parametric shapes.
 * Empty input yields empty output. */
Vector<PaintShape> shape_apply_canvas_symmetry(const PaintShape &shape,
                                               const CanvasTile &tile,
                                               const ToolSettings &tool_settings);

/** Every shape in \a shapes followed by its canvas-space symmetry copies. Empty input yields
 * empty output. */
Vector<PaintShape> shapes_expand_symmetry(Span<PaintShape> shapes,
                                          const CanvasTile &tile,
                                          const ToolSettings &tool_settings);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coordinate helpers
 * \{ */

/** UV origin (bottom-left corner) of the UDIM tile \a tile. */
float2 tile_uv_origin(int tile);

/** Reference-tile pixels <-> UV. */
float2 shape_px_to_uv(const CanvasTile &tile, const float2 &px);
float2 shape_uv_to_px(const CanvasTile &tile, const float2 &uv);

/** Image Editor View2D mapping: UV -> region pixels, region pixels -> UV, and reference-tile
 * pixels -> region pixels (through UV). */
float2 image_uv_to_region(const ARegion &region, const float2 &uv);
float2 image_region_to_uv(const ARegion &region, const float2 &region_px);
float2 shape_px_to_region(const ARegion &region, const CanvasTile &tile, const float2 &px);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Editor Vector session
 * \{ */

struct ImageShapeVectorState;

std::unique_ptr<VectorEditHost> image_shape_vector_host_create(bContext &C,
                                                               ImageShapeVectorState &session,
                                                               float tolerance_px);
ImageShapeVectorState *image_shape_vector_state_get(const SpaceImage *sima);
/** The live session's edit document in \a sima, or null when no session is floating. */
ShapeEditSession *image_shape_vector_edit_get(const SpaceImage *sima);
bool image_shape_vector_is_floating_in_space(const SpaceImage *sima);
/** Runtime "Transform" mode of the session in \a sima (shows the cage for Polygon). */
bool image_shape_vector_transform_mode_get(const SpaceImage *sima);
void image_shape_vector_transform_mode_toggle(SpaceImage *sima);
void image_shape_vector_session_begin(bContext *C,
                                      SpaceImage *sima,
                                      PaintShape shape,
                                      const CanvasTile &tile,
                                      const ShapeStyle &style);
void image_shape_vector_session_end_for_takeover(bContext *C, SpaceImage *sima);
void image_shape_vector_session_cancel(bContext *C, SpaceImage *sima);
void image_shape_vector_state_free(PaintSelectFloatingSession *session);
bool image_shape_vector_modal_active(const ImageShapeVectorState *state);
void image_shape_vector_modal_set_active(ImageShapeVectorState *state, bool active);

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
