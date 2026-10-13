/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The description of a layered material's stack: adding, removing, looking rows up and editing
 * them in place.
 *
 * A layered material keeps its stack as DNA (`Material::paint_layers`), not as a node graph.
 * Everything here is therefore description-only and never touches a node tree. Rows are addressed
 * by their #MaterialPaintLayer::marker, never by position: an index is ambiguous, while a marker
 * survives reordering and copying.
 *
 * Every mutation marks #MA_PAINT_LAYERS_REGEN and tags the material's shading. Setting the active
 * marker does not: it moves a cursor, not the stack.
 */

#include <cstdint>

#include "DNA_uuid_types.h"

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerChannel;
enum eMaterialPaintChannel : int8_t;
enum eMaterialPaintLayerBlend : int8_t;
enum eMaterialPaintLayerSource : int8_t;

/* -------------------------------------------------------------------- */
/** \name Layered material
 * \{ */

/**
 * Whether \a ma is a layered material: its stack is the DNA description (#MA_PAINT_LAYERED).
 *
 * This is the one flag test every guard shares, so a layered material is never read as a stack it
 * is not.
 */
bool BKE_paint_layers_is_layered(const Material &ma);

/** A description edit: whatever is derived from the description is stale until it is rebuilt. */
void BKE_paint_layers_tag_edited(Material &ma);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Description edits
 * \{ */

/**
 * Where #BKE_paint_layers_add puts the new row relative to its anchor.
 *
 * Mirrors the `place` names the RNA uses (`'ABOVE'` and `'BELOW'`).
 */
enum class PaintLayerPlace : int8_t {
  /** Directly above the anchor. */
  Above = 0,
  /** Directly below the anchor. */
  Below,
};

/**
 * Add a Paint row to \a ma.
 *
 * With a null \a anchor the row is appended on top of the stack; otherwise it is placed relative
 * to \a anchor by \a place. The row is given a fresh #bUUID marker unique within \a ma, and the
 * usual defaults: enabled, full opacity, no channels (see #BKE_paint_layers_default_channels_apply).
 *
 * Sets #MA_PAINT_LAYERED on \a ma -- a material that owns a description is a layered material --
 * and marks the material's derived state stale; see the file comment.
 *
 * \return the new row, or null when \a anchor is non-null but is not part of \a ma's stack.
 */
MaterialPaintLayer *BKE_paint_layers_add(Material &ma,
                                         const char *name,
                                         MaterialPaintLayer *anchor,
                                         PaintLayerPlace place);

/**
 * Deep-copy \a layer into a new row placed directly above it.
 *
 * The copy gets a fresh #bUUID marker unique within \a ma, a copy of the row's own state (flag,
 * opacity, name) and its channel records, and -- never the shared map -- a copy of each image a
 * channel references. The row is linked into \a ma's stack by the caller's insertion.
 *
 * \return the copy, or null when \a layer is null or not part of \a ma.
 */
MaterialPaintLayer *BKE_paint_layers_duplicate(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer *layer);

/**
 * Give a freshly authored Paint \a layer the channels it takes part in by default: an enabled
 * record with no map for Base Color, Metallic, Roughness, Normal and Ambient Occlusion.
 *
 * A row only reaches a channel through a channel record, so a layer with no records is inert.
 * Every authoring path -- `Material.paint_layers.new` and the shared Add policy -- calls this one
 * function, so the default set is decided in exactly one place. Existing records are left as they
 * are, so the call is idempotent.
 */
void BKE_paint_layers_default_channels_apply(Material &ma, MaterialPaintLayer &layer);

/**
 * Remove \a layer from \a ma, freeing it.
 *
 * When the active marker names the removed row, the active marker is cleared: a marker that
 * resolves to nothing is worse than no selection, and silently moving the selection to a neighbour
 * would hide that the row the user was working on is gone.
 *
 * \return false when \a layer is null or not part of \a ma.
 */
bool BKE_paint_layers_remove(Material &ma, MaterialPaintLayer *layer);

/**
 * Find the row of \a ma whose marker is \a marker, never by position, so a row keeps resolving
 * after a reorder.
 *
 * \return the row, or null when no row carries \a marker.
 */
MaterialPaintLayer *BKE_paint_layers_find(Material &ma, const bUUID &marker);

/** Set the active marker of \a ma; nil clears it. */
void BKE_paint_layers_active_set(Material &ma, const bUUID &marker);

/** The active marker of \a ma, nil when nothing is active. */
bUUID BKE_paint_layers_active_get(const Material &ma);

/**
 * The active row of \a ma (`Material::active_layer_marker`), or null when the marker is nil or
 * names no row.
 */
MaterialPaintLayer *BKE_paint_layers_active_layer_get(Material &ma);

/**
 * Move \a layer relative to \a anchor in \a ma's stack.
 *
 * Same placement rules as #BKE_paint_layers_add: a null anchor appends the row on top of the stack,
 * Above and Below are siblings of the anchor. Markers are untouched, so the active marker keeps
 * resolving.
 *
 * \return false when \a layer or \a anchor is not part of \a ma, or when \a anchor is \a layer
 * itself.
 */
bool BKE_paint_layers_move(Material &ma,
                           MaterialPaintLayer *layer,
                           MaterialPaintLayer *anchor,
                           PaintLayerPlace place);

/**
 * Move \a layer to \a index in the stack, bottom to top. The index is clamped to the list.
 *
 * \return false when \a layer is null or not part of \a ma.
 */
bool BKE_paint_layers_reorder(Material &ma, MaterialPaintLayer *layer, int index);

/**
 * Set the constant \a value of \a layer's \a channel, used while the channel carries no map.
 *
 * \return false when \a layer carries no \a channel record or is not part of \a ma.
 */
bool BKE_paint_layers_channel_set_value(Material &ma,
                                        MaterialPaintLayer *layer,
                                        eMaterialPaintChannel channel,
                                        const float value[4]);

/**
 * Point \a layer's \a channel at \a image, or detach it with a null one. User counts are
 * maintained here: the channel is one of the users that keeps the image alive.
 *
 * \return false when \a layer carries no \a channel record or is not part of \a ma.
 */
bool BKE_paint_layers_channel_set_image(Material &ma,
                                        MaterialPaintLayer *layer,
                                        eMaterialPaintChannel channel,
                                        Image *image);

/**
 * Add the \a channel record to \a layer: enabled, without a map, with a zero value. The map is
 * created by the first stroke into the channel, not here. An existing record for \a channel is
 * left untouched: channels are keyed by #eMaterialPaintChannel, so there is never a second record
 * for one channel to find.
 *
 * \return the record, or null when \a layer is null or not part of \a ma.
 */
MaterialPaintLayerChannel *BKE_paint_layers_channel_add(Material &ma,
                                                        MaterialPaintLayer *layer,
                                                        eMaterialPaintChannel channel);

/**
 * Remove the \a channel record of \a layer. Disabling keeps the map; removing forgets the channel
 * entirely.
 *
 * \return false when \a layer carries no \a channel record or is not part of \a ma.
 */
bool BKE_paint_layers_channel_remove(Material &ma,
                                     MaterialPaintLayer *layer,
                                     eMaterialPaintChannel channel);

/**
 * Set whether \a layer's \a channel takes part in the stack: enabled keeps the map live, disabled
 * keeps the map but switches it off. A missing record is refused -- adding one is an explicit
 * step.
 *
 * \return false when \a layer carries no \a channel record or is not part of \a ma.
 */
bool BKE_paint_layers_channel_set_enabled(Material &ma,
                                          MaterialPaintLayer *layer,
                                          eMaterialPaintChannel channel,
                                          bool enabled);

/** Set the opacity of \a layer. */
bool BKE_paint_layers_set_opacity(Material &ma, MaterialPaintLayer *layer, float opacity);

/** Set whether \a layer takes part in the stack. */
bool BKE_paint_layers_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled);

/** Rename \a layer. */
bool BKE_paint_layers_rename(Material &ma, MaterialPaintLayer *layer, const char *name);

/**
 * Set how \a layer combines with what is below it; see #eMaterialPaintLayerBlend.
 *
 * #MA_PAINT_LAYER_BLEND_NORMAL_COMBINE is refused: the Normal channel forces its operation on its
 * own, so a stored value would only ever be a switch the user cannot make effective.
 */
bool BKE_paint_layers_set_blend(Material &ma,
                                MaterialPaintLayer *layer,
                                eMaterialPaintLayerBlend blend);

/**
 * Change what \a layer paints with between a painted map (#MA_PAINT_LAYER_SOURCE_IMAGE) and a flat
 * colour (#MA_PAINT_LAYER_SOURCE_CONSTANT, a Fill). What the two disagree on is converted: the maps
 * are let go when the row becomes a Fill (a transparent Base Color becomes opaque black), and the
 * Base Color goes back to transparent when it becomes Paint again, so the row lays nothing until
 * it is painted.
 */
bool BKE_paint_layers_set_source(Material &ma,
                                 MaterialPaintLayer *layer,
                                 eMaterialPaintLayerSource source);

/**
 * The Base Color \a layer shows: the value of its Base-Color channel record, which is where a Fill's
 * colour lives (a Layer row keeps every channel, Base Color included, in records). Zero when the row
 * has no Base-Color record.
 *
 * The one reader of the Base-Color constant for code that has only the row, not a channel index --
 * the Outliner swatch and the RNA `fill_color` property.
 */
void BKE_paint_layers_base_color_get(const MaterialPaintLayer &layer, float r_color[4]);

/**
 * Whether the Base Color #BKE_paint_layers_base_color_get reads is live: the row's Base-Color record
 * exists and is ENABLED. The UI asks this before drawing a colour control that would stand for a
 * channel the row no longer paints with.
 */
bool BKE_paint_layers_base_color_active(const MaterialPaintLayer &layer);

/** Set the fill color of \a layer, the constant a Fill shows; creates its Base-Color record on demand. */
bool BKE_paint_layers_set_fill_color(Material &ma, MaterialPaintLayer *layer, const float color[4]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Values the stack is evaluated with
 * \{ */

/**
 * The opacity \a layer blends by: its own opacity, or zero while the row is switched off.
 *
 * The one reader of "how much does this row show", for every evaluator of the description.
 */
float BKE_paint_layers_effective_opacity(const MaterialPaintLayer &layer);

/**
 * The value \a channel's stack starts from, before any row is laid over it: the shader-space RGBA
 * a CPU composite must initialise its buffer to.
 *
 * The values are the channel's neutral default -- what the Principled BSDF shows where nothing
 * covers it -- and the alpha is used only where a channel has one. Every evaluator reads this one
 * table, or a partially covered row would fade towards different values in each of them.
 */
void BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel channel, float r_color[4]);

/** \} */

}  // namespace blender
