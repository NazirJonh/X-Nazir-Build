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
 * The Pixel operators resolve their targets from the context (#shape_bake).
 */

#pragma once

#include <cstdint>

#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_image_types.h"

#include "paint_image_shape.hh"

namespace blender {

struct bContext;
struct Image;
struct ImageUser;
struct ImBuf;
struct Object;
struct Paint;
struct PaintMaterialImageTarget;
struct ToolSettings;

}  // namespace blender

namespace blender::ed::sculpt_paint::shape {

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
 * Run once when a draw starts (the Pixel operator's invoke), before
 * #composite_targets_get resolves them: the write paths themselves must never allocate images,
 * since they run outside any undo step that would record the creation.
 *
 * \param paint: the channel source for this context -- `tool_settings.imapaint.paint` in the
 * Image Editor.
 * \param settings: decides the channel set (Override Channels, per-part `use`): the shared tool
 * settings block for a Pixel draw.
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
 * \note The channel set is resolved by #BKE_paint_shape_target_channels, the same rule the
 * tool UI (`PaintShapeSettings.target_channels`) uses.
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
 * The Pixel bake applies it right before compositing.
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
 * The shapes are already symmetry-expanded, so callers pass the output of #shape_bake; their
 * pixels all refer to the reference tile. Tile regions are the union over every expanded shape.
 *
 * The shapes are defined in reference-tile pixels and each target tile rasterizes them at its
 * own resolution: a tile of a different size than #CanvasTile::ref_tile_size gets the geometry
 * and the pixel-based style widths scaled (#shapes_scale_to_target), so a channel map of half
 * the canvas resolution receives the same UV-space shape.
 *
 * With \a push_undo the write collapses into one labeled image undo step, the images are tagged
 * for the depsgraph and \a ob's material shading refreshes; without it only the touched
 * regions are marked and the notifier stays light.
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
 * The Pixel path.
 */
bool shape_bake(bContext *C,
                Span<PaintShape> shapes,
                const CanvasTile &tile,
                const ShapeStyle &style,
                bool push_undo,
                const char *undo_name);

/** Full-tile snapshot of one target tile (the image undo source). */
ImBuf *shape_tile_backup_init(Image *image, const ImageUser *owner_iuser, int tile_number);
/** Tile numbers of \a image whose area the (already symmetry-expanded) shapes reach. */
Vector<int> composite_affected_tiles_get(Image *image,
                                         const ImageUser *owner_iuser,
                                         Span<PaintShape> shapes,
                                         const CanvasTile &tile,
                                         const ShapeStyle &style);

/** Expand \a r_region to also cover \a other (both half-open tile rects, either may be empty). */
void shape_region_union(rcti &r_region, const rcti &other);
/** Tile-local pixel rect the (already symmetry-expanded) shapes cover on \a tile, half-open and
 * clipped to the tile and the selection mask. Empty rect when nothing is covered. */
rcti composite_tile_region_get(Image *image,
                               const ImageUser *owner_iuser,
                               int tile_number,
                               Span<PaintShape> shapes,
                               const CanvasTile &tile,
                               const ShapeStyle &style);

}  // namespace blender::ed::sculpt_paint::shape
