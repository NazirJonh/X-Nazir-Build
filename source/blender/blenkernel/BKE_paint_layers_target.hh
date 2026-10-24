/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * \file
 * \ingroup bke
 *
 * The paint target of a layered material: which description row, and which of its channel maps, a
 * brush stroke writes into.
 *
 * The target is a cursor, not a stored binding: it is `Material::active_layer_marker` resolved to a
 * row, plus the channel the stroke writes. A stroke and the UI both read it through this one
 * resolver, so a layered material stores no per-channel image binding: the description is the only
 * source of what a channel paints into.
 *
 * The first stroke into a channel the row has no map for is where the description grows:
 * #BKE_paint_layers_target_ensure_writable adds the record, if the row has none, and its map.
 * Creating the map is an ID creation plus a description edit, so it belongs in the stroke's invoke
 * on the main thread, before the stroke's undo step opens. It is a persistent side effect and is
 * deliberately not wrapped into that step, so cancelling or undoing the stroke keeps the map; this
 * API never opens undo and never runs from a sample.
 */

#include <cstdint>

#include "BLI_span.hh"

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerChannel;
struct Object;
struct PaintModeSettings;
struct ReportList;

/**
 * Where a description references one image: the row whose `channels` carries it and the
 * #eMaterialPaintChannel it drives.
 *
 * Identity by description, never by #Image::paint_layer_id: the map belongs to the row whose
 * `channels` holds it, so a map used by two materials stays with the one that was searched.
 */
struct PaintLayersImageUse {
  /** The row, its content correction, or the mask item (see #is_mask), whose `channels` carries the
   * image. */
  MaterialPaintLayer *layer = nullptr;
  /** The #eMaterialPaintChannel the image drives. */
  int channel = -1;
  /** The image is the map of a mask item, not channel content of a row. */
  bool is_mask = false;
};

/**
 * Find where \a ma's description references \a image.
 *
 * Searches \a ma alone: an image shared by two materials resolves to the row of the material that
 * was asked about, so a caller never has to disambiguate by a stored back-pointer.
 *
 * \return false when \a ma's description does not carry \a image.
 */
bool BKE_paint_layers_find_image_use(Material &ma,
                                     const Image &image,
                                     PaintLayersImageUse &r_use);

/** What a stroke of a layered material paints: the row's channel content, or its mask. */
enum class PaintLayersTargetMode : int8_t {
  Content = 0,
  Mask = 1,
};

/**
 * Why a paint target did or did not resolve. The boolean #BKE_paint_layers_target_get overloads
 * collapse every refusal into false; callers that react differently to a refusal (skip one channel
 * versus give up on the whole stroke) ask #BKE_paint_layers_target_resolve instead of re-deriving
 * the reason from the material themselves.
 */
enum class PaintLayersTargetStatus : int8_t {
  /** The target resolved. Not a promise that a record or map exists: #PaintLayersTarget::channel_record
   * stays null while the row has no record for the channel (it is created on the first stroke), so
   * callers that need an existing record check #BKE_paint_layers_target_is_live. */
  Ok = 0,
  /** No material, or it is not a layered one. */
  NotLayered,
  /** The channel index is out of range. */
  InvalidChannel,
  /** The material has no active row. */
  NoActiveRow,
  /** Content: the active row owns no map a stroke could write (a filter, or a row that reads a
   * material). */
  RowTakesNoStrokes,
  /** Content: the channel is not in the material's channel set, so it takes part nowhere. This is
   * the user's choice and concerns this channel alone, not the rest of the stroke. */
  OutsideChannelSet,
  /** Mask: only Base Color names the mask map, so any other channel has no target. */
  ChannelNotInMode,
  /** Mask: the row has no mask, or its kind is not a stroke target. */
  NoMask,
  /** The row, the stack row a mask item or correction hangs on, or one of its folders is locked. */
  Locked,
};

/** The resolved description row and channel a stroke paints into. */
struct PaintLayersTarget {
  /** The material owning the row; never null when the resolver succeeded. */
  Material *material = nullptr;
  /** The active row. */
  MaterialPaintLayer *layer = nullptr;
  /**
   * The row's record for #channel, or null while the row has none.
   *
   * Points into the row's `channels` array, which adding a record reallocates: resolve the target
   * again after any edit of the row's channels.
   */
  MaterialPaintLayerChannel *channel_record = nullptr;
  /** #eMaterialPaintChannel the target addresses. */
  int channel = 0;
  /** What the stroke paints: the row's channel content, or its mask. */
  PaintLayersTargetMode mode = PaintLayersTargetMode::Content;
  /**
   * The mask item a #PaintLayersTargetMode::Mask target paints: the row's base mask. Its Base Color
   * record is #channel_record, and its map the one the stroke writes.
   */
  MaterialPaintLayer *mask_item = nullptr;
  /**
   * Set by the object-based resolvers when the row is hidden on that object (object mask in Mesh
   * mode, the row's or an enclosing folder's): a stroke through the object writes nothing.
   */
  bool object_masked = false;
  /**
   * UDIM tiles (bit `i` = tile `1001 + i`) the row is hidden on for that object (object mask in UV
   * Tiles mode): a stroke skips them. Set by the object-based resolvers only.
   */
  uint32_t excluded_tiles = 0;
};

/**
 * Resolve the paint target of \a ma's active row for \a channel.
 *
 * \return false when \a ma is not a layered material, \a channel is out of range, has no active
 * marker, or the marker names no row. A row without a record for \a channel still resolves: the
 * record is created on the first stroke.
 */
bool BKE_paint_layers_target_get(Material &ma, int channel, PaintLayersTarget &r_target);

/**
 * Resolve the target of \a ma's active row for \a mode.
 *
 * In #PaintLayersTargetMode::Mask the target is the row's base mask, and only Base Color names it:
 * a mask is one grey map, so a stroke's other channels resolve to nothing instead of being written
 * into it.
 *
 * In #PaintLayersTargetMode::Content a channel outside #Material::paint_layers_channels resolves to
 * nothing: the material's set is the filter over what the stack paints, and a stroke into such a
 * channel would be invisible work. Mask mode ignores the set, because a mask is not a content
 * channel.
 *
 * \return false as #BKE_paint_layers_target_get does, when \a channel is outside the material's
 * channel set (Content), and in Mask mode also when \a channel is not Base Color or the row has no
 * mask.
 */
bool BKE_paint_layers_target_get(Material &ma,
                                 int channel,
                                 PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target);

/**
 * As #BKE_paint_layers_target_get for \a mode, reporting why a target is refused. \a r_target is
 * only meaningful when #PaintLayersTargetStatus::Ok is returned.
 */
PaintLayersTargetStatus BKE_paint_layers_target_resolve(Material &ma,
                                                        int channel,
                                                        PaintLayersTargetMode mode,
                                                        PaintLayersTarget &r_target);

/** The mode a stroke runs in: #PaintModeSettings::layer_target_mode, Content for null. */
PaintLayersTargetMode BKE_paint_layers_target_mode(const PaintModeSettings *settings);

/**
 * The layered material \a ob paints into: its active material slot's material, when that material
 * is layered.
 *
 * This is the one place the "active layered material" is chosen, matching the slot selection of
 * #BKE_paint_layers_target_get; it never scans #Main.
 *
 * \return null when \a ob is null, its active slot is empty, or the material is not layered.
 */
Material *BKE_paint_layers_active_material_get(Object *ob);

/**
 * Resolve through an object and one of its material slots, the path the brush and the UI share.
 * \a material_slot is the 0-based slot index; a negative value means the active slot.
 *
 * \return false when the slot has no material or the material is not a target (see
 * #BKE_paint_layers_target_get).
 */
bool BKE_paint_layers_target_get(
    Object &ob, int material_slot, int channel, PaintLayersTarget &r_target);

/** As above, resolving the target of \a mode. */
bool BKE_paint_layers_target_get(Object &ob,
                                 int material_slot,
                                 int channel,
                                 PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target);

/** As #BKE_paint_layers_target_resolve, through an object and one of its material slots. */
PaintLayersTargetStatus BKE_paint_layers_target_resolve(Object &ob,
                                                        int material_slot,
                                                        int channel,
                                                        PaintLayersTargetMode mode,
                                                        PaintLayersTarget &r_target);

/**
 * Whether the active row takes strokes into the channel: it has a record for it and the record is
 * switched on.
 *
 * A brush that writes the channel leaves a row without a record, or with a disabled one, alone: the
 * row's channels are the user's choice, and a stroke never adds or re-enables one on its own.
 */
bool BKE_paint_layers_target_is_live(const PaintLayersTarget &target);

/**
 * The map \a target paints into, or null while it has none.
 *
 * A disabled record keeps its map for the file but is not a paint target, so it reads as having
 * none.
 */
Image *BKE_paint_layers_target_image(const PaintLayersTarget &target);

/**
 * Make \a target hold a writable map, growing the description on the first stroke.
 *
 * The row gets its channel record when it has none, and the record a map filled with the value the
 * record carries (a fresh record, one the row did not have, starts from the channel's neutral:
 * opaque black, or the flat tangent for Normal). New maps are straight-alpha RGBA in the channel's
 * colorspace (sRGB for colour, Non-Color for the scalar and normal channels). This is an ID
 * creation plus a description edit, so it runs on the main thread before the stroke opens its undo
 * step. The created map is a persistent side effect and is not part of that step, so cancelling or
 * undoing the stroke keeps it. It never opens undo itself.
 *
 * \param image_size: the fallback width and height, used only when the material has no size of
 * its own (#Material::paint_layers_image_size is zero).
 * \param new_channel_fill_linear: scene-linear RGBA a record created here starts from, instead of
 * the channel's neutral.
 * \return the map, or null when \a target carries nothing writable: no row, or a record that is
 * switched off, which a stroke never switches back on behind the user's back.
 */
Image *BKE_paint_layers_target_ensure_writable(Main &bmain,
                                               PaintLayersTarget &target,
                                               int image_size,
                                               const float *new_channel_fill_linear = nullptr);

/**
 * Give the active row a map for every channel of the material's global set that the row takes part
 * in and has no map for yet, so a fresh layer is paintable in all of them at once instead of
 * growing one stroke at a time (and the UI never offers to create missing maps for it).
 *
 * Same ID-creation rules as #BKE_paint_layers_target_ensure_writable. \a image_size is the fallback
 * when the material has no size of its own.
 * \return the number of maps created.
 */
int BKE_paint_layers_active_maps_ensure(Main &bmain, Material &ma, int image_size);

/**
 * The Base Color map of the map-coverage mask item \a item, created zero-filled on the first call.
 *
 * The map is the coverage the operator samples (`match`): a scalar for UV Island, an RGBA the Color
 * ID comparisons read. Only a kind whose map is paintable takes one; a reference-only kind (Color
 * ID, an external ID Map) refuses, so the description never grows a map the user did not assign.
 * The name follows the mask maps' `"<layer> <item>"` convention, and the map is straight-alpha
 * Non-Color data, like every other paint-layer map.
 *
 * \param image_size: the fallback width and height, used only when the material has no size of its
 * own (#Material::paint_layers_image_size is zero).
 * \return the map, or null when \a item is not a paintable map-coverage mask item of \a ma, or the
 * description edit to attach the map failed.
 */
Image *BKE_paint_layers_map_coverage_map_ensure(Main &bmain,
                                                Material &ma,
                                                MaterialPaintLayer &item,
                                                int image_size);

/**
 * Make sure the tiled coverage map \a image has UDIM tile \a tile_number, adding it zero-filled and
 * shaped like the first tile when it is missing.
 * \return false when \a image is not a tiled image or the tile cannot be made.
 */
bool BKE_paint_layers_map_coverage_tile_ensure(Image &image, int tile_number);

/**
 * Collapse the mask stack of \a layer (its base mask and every correction over it) into one map,
 * which becomes the base's own: the corrections are removed, and the base reads the merged map
 * with the canonical base settings restored -- Multiply, full opacity, no invert, identity
 * mapping. The map holds the factor the strokes and the composite read (#PaintLayersMaskSampler),
 * 8-bit like every other mask map, so the row's look changes only by that quantization.
 *
 * Refused, with the reason reported to \a reports, when there is nothing to fold (no corrections),
 * when the base is a filter or a coverage operator (its kind has no map to read), when a lock
 * blocks the row -- its own, a mask item's, or a folder's above, the same lock a stroke answers --
 * or when maps that are read at their own size disagree on that size. A mapped item is resampled
 * to the merged size, so its own never conflicts. UDIM stacks merge tile by tile: the union of the
 * stack's tiles becomes the merged map's tiles, and a tile an item's map does not have reads as
 * that item's constant, the same fallback the render shows for it.
 *
 * Nothing of the description changes until the merged map is written and attached, so a refusal
 * leaves the material exactly as it was. The maps the stack held lose their records' users and
 * become orphans the usual way. The merge is one-way and this API never opens undo: the caller
 * owns that story.
 *
 * \return the merged map, or null on refusal.
 */
Image *BKE_paint_layers_mask_stack_merge(Main &bmain,
                                         Material &ma,
                                         MaterialPaintLayer &layer,
                                         ReportList *reports);

/**
 * What baking \a selected asks of the user: 0 when it cannot be baked as selected (not corrections
 * of one row, not next to each other, a kind with no colour of its own, or a Paint correction on its
 * own, which is a texture already), 1 for a single Fill or Material correction (a conversion to a
 * texture), 2 for two or more (a merge). A shape test for menus: locks and blends are left to
 * #BKE_paint_layers_corrections_bake, which reports them.
 */
int BKE_paint_layers_corrections_bake_kind(Material &ma, Span<MaterialPaintLayer *> selected);

/**
 * The refusals of #BKE_paint_layers_corrections_bake that need no render, answered up front: shape,
 * locks, a Paint correction on its own, the blend of the lowest correction of a channel. A caller
 * that renders Material corrections first asks this before paying for the render. A refusal is
 * reported to \a reports. A Material correction that is not rendered yet is not a refusal here.
 */
bool BKE_paint_layers_corrections_bake_precheck(Material &ma,
                                                Span<MaterialPaintLayer *> selected,
                                                ReportList *reports);
/**
 * Bake the content corrections \a selected (one row's, next to each other) into a single Paint
 * correction: a map per channel they lay in, holding what they lay together over nothing, stacked so
 * that the upper covers the lower as the render shows it. Fill and Material corrections become
 * textures this way, and so does one of them on its own.
 *
 * The new correction takes the place of the topmost of \a selected, blends with Normal at full
 * opacity, and becomes the active row; the corrections it replaces are removed and their maps become
 * orphans. UDIM maps are baked tile by tile.
 *
 * Refused, with the reason reported to \a reports, when the shape is wrong (see
 * #BKE_paint_layers_corrections_bake_kind), a lock blocks one of them (the same lock a stroke
 * answers to), the lowest correction of a channel is not blended with Normal (baked, it would lose
 * the colour it read below), or the maps cannot be read at one size. Nothing of the description
 * changes until every map is written, so a refusal leaves the material as it was. The bake is
 * one-way and this API never opens undo: the caller owns that story.
 *
 * \return the new correction, or null on refusal.
 */
MaterialPaintLayer *BKE_paint_layers_corrections_bake(Main &bmain,
                                                      Material &ma,
                                                      Span<MaterialPaintLayer *> selected,
                                                      ReportList *reports);

/**
 * Free the maps a one-way bake or merge left without a user once the shader tree that still held
 * them has been rebuilt (see #BKE_paint_layers_corrections_bake). Cheap when nothing is waiting:
 * call it on the main thread after the depsgraph update.
 */
void BKE_paint_layers_orphan_maps_purge(Main &bmain);
}  // namespace blender
