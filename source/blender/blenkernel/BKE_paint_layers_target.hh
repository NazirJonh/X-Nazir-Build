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
 * on the main thread, wrapped in one undo group together with the stroke itself; this API never
 * opens undo and never runs from a sample.
 */

#include <cstdint>

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerChannel;
struct Object;
struct PaintModeSettings;

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
 * \return false as #BKE_paint_layers_target_get does, and in Mask mode also when \a channel is not
 * Base Color or the row has no mask.
 */
bool BKE_paint_layers_target_get(Material &ma,
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
 * creation plus a description edit, so it runs on the main thread, inside the stroke's undo group;
 * it never opens undo itself.
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

}  // namespace blender
