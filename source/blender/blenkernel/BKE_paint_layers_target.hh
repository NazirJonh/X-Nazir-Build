/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * \file
 * \ingroup bke
 *
 * The paint target of a layered material: which description row a brush stroke writes into.
 *
 * The target is a cursor, not a stored binding: it is `Material::active_layer_marker` resolved to a
 * row, plus the active channel and the content/mask mode. A stroke and the UI both read it through
 * this one resolver, so a layered material stores no per-channel image binding: the description is
 * the only source.
 *
 * The first stroke into a channel or mask the row does not have yet is where the description grows:
 * #BKE_paint_layers_target_ensure_writable adds the record and its map, converting a Fill row to
 * Paint as the design's C-3 describes. Creating the map is an ID-creation plus a description edit,
 * so it belongs in the stroke's invoke on the main thread, wrapped in one undo group together with
 * the stroke itself; this API never opens undo and never runs from a sample.
 */

#include <cstdint>

#include "DNA_uuid_types.h"

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerChannel;
struct Object;
struct PaintModeSettings;

/** What part of a description row an image belongs to, as #BKE_paint_layers_find_image_use reads. */
enum class PaintLayersImageUseRole : int8_t {
  /** The image is a channel map of the row: `channels[].image`. */
  Content = 0,
  /** The image is the row's mask map: a mask item's Base-Color channel record. */
  Mask,
  /** The image is a channel map of a correction row hanging on the row. */
  Correction,
};

/**
 * Where a description references one image: the row that owns it and, for a correction, the
 * correction's marker.
 *
 * Identity by description, never by #Image::paint_layer_id: the map belongs to the row whose
 * `channels` or mask-item record holds it, so a map used by two materials stays with the one that
 * was searched.
 */
struct PaintLayersImageUse {
  /** The row whose channel or mask carries the image; for Correction, the correction row itself. */
  MaterialPaintLayer *layer = nullptr;
  /** The #eMaterialPaintChannel the image drives, or #PAINT_LAYER_MAP_MASK for a mask. */
  int channel = -1;
  PaintLayersImageUseRole role = PaintLayersImageUseRole::Content;
  /** The correction row's marker for #PaintLayersImageUseRole::Correction, nil otherwise. */
  bUUID correction = {};
};

/**
 * Find where \a ma's description references \a image, searching rows, their masks, their
 * corrections and nested children.
 *
 * Searches \a ma alone: an image shared by two materials resolves to the row of the material that
 * was asked about, so a caller never has to disambiguate by a stored back-pointer.
 *
 * \return false when \a ma's description does not carry \a image.
 */
bool BKE_paint_layers_find_image_use(Material &ma,
                                     const Image &image,
                                     PaintLayersImageUse &r_use);

/** Which part of the active row a paint target addresses. */
enum class PaintLayersTargetMode : int8_t {
  /** A channel of the row: its map, or the constant it will paint over. */
  Content = 0,
  /** The row's mask. */
  Mask = 1,
};

/** The resolved description row a stroke paints into. */
struct PaintLayersTarget {
  /** The material owning the row; never null when the resolver succeeded. */
  Material *material = nullptr;
  /** The active row. May be a correction, like any description row. */
  MaterialPaintLayer *layer = nullptr;
  /** Content mode: the row's record for #channel, or null while the row has none. */
  MaterialPaintLayerChannel *channel_record = nullptr;
  /**
   * Mask mode: the mask item the stroke writes into, or null while the row has none. The active
   * mask item is the row selected in the Outliner when it is itself a mask item, otherwise the
   * first element of the active row's mask stack.
   */
  MaterialPaintLayer *mask_item = nullptr;
  /** #eMaterialPaintChannel the target addresses. */
  int channel = 0;
  PaintLayersTargetMode mode = PaintLayersTargetMode::Content;
  /**
   * Empty when the object the target was resolved through has the UV layer its material names.
   * Otherwise the reason a stroke is refused, formatted at resolution time because only the
   * object-based resolver knows the mesh.
   */
  char uv_refusal[160] = {};
};

/**
 * Resolve the paint target of \a ma's active row for \a channel and \a mode.
 *
 * \return false when \a ma is not a layered material, has no active marker, the marker names no
 * row, or the row is a folder (folders carry no maps). A row without a record for \a channel still
 * resolves: the record is created on the first stroke.
 */
bool BKE_paint_layers_target_get(Material &ma,
                                 int channel,
                                 PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target);

/**
 * The layered material \a ob paints into: its active material slot's material, when that material
 * is layered.
 *
 * This is the one place the "active layered material" is chosen, matching the slot selection of
 * #BKE_paint_layers_target_get; it never scans #Main, unlike the old path's binding search.
 *
 * \return null when \a ob is null, its active slot is empty, or the material is not layered.
 */
Material *BKE_paint_layers_active_material_get(Object *ob);

/**
 * The active row of layered \a ma (`Material::active_layer_marker`), or null when the marker is
 * nil or names no row. May be a correction, like any description row; a folder is returned too, so
 * a caller that cannot act on one checks #BKE_paint_layers_is_folder.
 */
MaterialPaintLayer *BKE_paint_layers_active_layer_get(Material &ma);

/**
 * Resolve through an object and one of its material slots, the path the brush and the Outliner
 * share. \a material_slot is the 0-based slot index; a negative value means the active slot.
 *
 * \return false when the slot has no material or the material is not a target (see
 * #BKE_paint_layers_target_get).
 */
bool BKE_paint_layers_target_get(Object &ob,
                                 int material_slot,
                                 int channel,
                                 PaintLayersTargetMode mode,
                                 PaintLayersTarget &r_target);

/**
 * Same, taking the mode from #PaintModeSettings::layer_target_mode -- the path a stroke uses, so
 * the mode never has to be passed around separately.
 */
bool BKE_paint_layers_target_get(Object &ob,
                                 int material_slot,
                                 int channel,
                                 const PaintModeSettings &mode_settings,
                                 PaintLayersTarget &r_target);

/** The map \a target paints into, or null while it has none. */
Image *BKE_paint_layers_target_image(const PaintLayersTarget &target);

/**
 * Whether \a target is frozen by a bake and cannot be painted.
 *
 * A stub until the bake cache of phase 3 lands, when it will answer from the node's bake state.
 * The policy is fixed now: a frozen target refuses the stroke with a message rather than
 * unfreezing itself behind the user's back.
 */
bool BKE_paint_layers_target_is_frozen(const PaintLayersTarget &target);

/**
 * Why a stroke into \a target is refused, or null when it may be painted: the target is frozen by
 * a bake, or it is the content of a Fill or Material row. A Fill is a colour and a Material row
 * shows its source's bake: their look is changed by a Correction painted on top, and only their
 * mask takes strokes. Every stroke entry point asks this
 * before anything is created (C-1), so a refused stroke leaves the description untouched.
 */
const char *BKE_paint_layers_target_refusal(const PaintLayersTarget &target);

/**
 * Make \a target hold a writable map, growing the description on the first stroke.
 *
 * Content mode: a Paint row gets its channel record and a map filled with the channel's current
 * constant. A Fill row is a colour and gets no map (null): see #BKE_paint_layers_target_refusal.
 * Mask mode: the target's mask item, created as the row's first mask element (a constant of one)
 * when it has none, gets a Base-Color channel record and a map. A constant element's map is opaque
 * and filled with its constant, so the mask keeps its look once painted; a painted element's map
 * starts transparent, so an unpainted item changes nothing.
 *
 * New maps are straight-alpha RGBA in the channel's colorspace (sRGB for colour, Non-Color for the
 * scalar and normal channels). This is an ID creation plus a description edit, so it runs on the
 * main thread, inside the stroke's undo group; it never opens undo itself.
 *
 * \return the map, or null when \a target carries nothing writable.
 */
Image *BKE_paint_layers_target_ensure_writable(Main &bmain,
                                               PaintLayersTarget &target,
                                               int image_size,
                                               const float *new_channel_fill_linear = nullptr);

}  // namespace blender
