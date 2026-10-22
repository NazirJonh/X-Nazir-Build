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
#include <memory>

#include "BLI_function_ref.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_listBase.h"
#include "DNA_uuid_types.h"

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerChannel;
struct MaterialPaintLayerIdColor;
struct MaterialSourceResolve;
struct ReportList;
enum eMaterialPaintChannel : int8_t;
enum eMaterialPaintLayerBlend : int8_t;
enum eMaterialPaintLayerFilter : int16_t;
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

/**
 * A strictly value-only edit (a row's opacity, a Fill row's constant): the generated tree keeps its
 * nodes and only its values are re-synced, so no shader rebuild happens. Falls back to
 * #BKE_paint_layers_tag_edited when the tree no longer matches the description's values.
 */
void BKE_paint_layers_tag_value_edited(Material &ma);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Folders
 * \{ */

/**
 * Whether \a layer is a folder: a row that holds its stack in #MaterialPaintLayer::children and lays
 * their isolated result over what is below.
 *
 * The one test for folder-ness everywhere -- the generator, the CPU composite, the channel rules and
 * the UI -- so a row can never be a folder on one side and a leaf on another. It reads
 * #MaterialPaintLayer::source (#MA_PAINT_LAYER_SOURCE_STACK), never a non-empty children list: an
 * emptied folder stays a folder.
 */
bool BKE_paint_layers_is_folder(const MaterialPaintLayer &layer);

/** Whether the folder \a layer holds any row (an emptied folder reports false). */
bool BKE_paint_layers_has_children(const MaterialPaintLayer &layer);

/**
 * Whether \a folder is a Pass Through folder: a folder that adds nothing but grouping, so its rows
 * are laid in the stack exactly as if it were not there.
 *
 * True for a folder with no mask and no content correction, the Mix blend and full opacity (a
 * mask or a correction folds a factor or a colour into the folder's own row, which an inlined
 * folder has none of). Visibility is deliberately not
 * part of it: a hidden Pass Through folder has no row to carry its visibility, so it multiplies its
 * rows' opacity by zero instead, which is a value edit, not a different graph. The one reader of
 * the mode, for the generator and the CPU composite alike.
 */
bool BKE_paint_layers_folder_is_pass_through(const MaterialPaintLayer &folder);

/**
 * Visit every row of \a ma's stack, depth first: a row, then -- for a folder -- the rows it holds,
 * each with its parent folder (null at the top level). This is the one order the Outliner numbers
 * rows by and every nested walk of the description uses.
 *
 * Mask items are not rows of the stack and are not visited. Return false from \a fn to stop.
 */
void BKE_paint_layers_foreach(
    const Material &ma,
    FunctionRef<bool(const MaterialPaintLayer &layer, const MaterialPaintLayer *parent)> fn);
void BKE_paint_layers_foreach(
    Material &ma, FunctionRef<bool(MaterialPaintLayer &layer, MaterialPaintLayer *parent)> fn);

/**
 * The list of \a ma's stack that directly holds \a layer -- the top-level list or a folder's
 * children, at any depth -- or null when \a layer is not part of the stack.
 */
ListBaseT<MaterialPaintLayer> *BKE_paint_layers_owner_list(Material &ma,
                                                           const MaterialPaintLayer &layer);

/** The folder that holds \a layer, or null when it hangs on the top level (or is not in \a ma). */
MaterialPaintLayer *BKE_paint_layers_parent(Material &ma, const MaterialPaintLayer &layer);

/** Whether \a marker names \a root itself or any row nested under it. */
bool BKE_paint_layers_subtree_contains(const MaterialPaintLayer &root, const bUUID &marker);

/**
 * Fold \a layers into a new folder row, which takes the position of the lowest member. The rows
 * keep their markers, their order and their own subtrees.
 *
 * \return the folder, or null when \a layers is empty or its rows do not all hang on one and the
 * same list of \a ma's stack (grouping across nesting levels is refused).
 */
MaterialPaintLayer *BKE_paint_layers_group(Material &ma, Span<MaterialPaintLayer *> layers);

/**
 * Lift the rows held by \a folder into the list that owns it, at its position and in their order,
 * and free the folder itself. The rows keep their markers and subtrees.
 *
 * \return false when \a folder is null, not part of \a ma, or not a folder.
 */
bool BKE_paint_layers_ungroup(Material &ma, MaterialPaintLayer *folder);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Description edits
 * \{ */

/**
 * Where #BKE_paint_layers_add puts the new row relative to its anchor.
 *
 * Mirrors the `place` names the RNA uses (`'ABOVE'`, `'BELOW'` and `'INTO'`).
 */
enum class PaintLayerPlace : int8_t {
  /** Directly above the anchor. */
  Above = 0,
  /** Directly below the anchor. */
  Below,
  /**
   * Inside the anchor, which has to be a folder: on top of what it holds. A place of its own rather
   * than "above the folder's top row", because an empty folder has no row to name -- and an empty
   * folder is exactly what a folder is when it is made to be filled.
   */
  Into,
};

/**
 * Add a Paint row to \a ma.
 *
 * With a null \a anchor the row is appended on top of the stack; otherwise it is placed relative
 * to \a anchor by \a place, in the list that holds the anchor. #PaintLayerPlace::Into requires the
 * anchor to be a folder (#BKE_paint_layers_is_folder): the row goes on top of what it holds. Nothing
 * here turns a plain row into a folder, so a caller that wants "into a plain row" groups instead
 * (#BKE_paint_layers_group). The row is given a fresh #bUUID marker unique within \a ma, and the
 * usual defaults: enabled, full opacity, no channels (see #BKE_paint_layers_default_channels_apply).
 *
 * Sets #MA_PAINT_LAYERED on \a ma -- a material that owns a description is a layered material --
 * and marks the material's derived state stale; see the file comment.
 *
 * \return the new row, or null when \a anchor is non-null but is not part of \a ma's stack, or
 * when \a place is Into and \a anchor is not a folder.
 */
MaterialPaintLayer *BKE_paint_layers_add(Material &ma,
                                         const char *name,
                                         MaterialPaintLayer *anchor,
                                         PaintLayerPlace place);

/**
 * Deep-copy the branch of \a layer into a new row placed directly above it, in the list that holds
 * it.
 *
 * Every row of the branch -- a folder's children included -- gets a fresh #bUUID marker unique
 * within \a ma, a copy of the row's own state (flag, opacity, name) and its channel records, and --
 * never the shared map -- a copy of each image a channel references.
 *
 * \return the copy, or null when \a layer is null or not part of \a ma.
 */
MaterialPaintLayer *BKE_paint_layers_duplicate(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer *layer);

/**
 * Give a freshly authored Paint or Fill \a layer the channels it takes part in by default: an
 * enabled record with no map for every channel of the material's global set
 * (#BKE_paint_layers_channel_set_mask_get).
 *
 * A row only reaches a channel through a channel record, so a layer with no records is inert.
 * Every authoring path -- `Material.paint_layers.new`, `MATERIAL_OT_new_layered` and the shared Add
 * policy -- calls this one function, so the default set is decided in exactly one place. Existing
 * records are left as they are, so the call is idempotent. A set that was never authored derives
 * from the layer records and the build default, so this is also how the build default reaches a
 * fresh stack.
 */
void BKE_paint_layers_default_channels_apply(Material &ma, MaterialPaintLayer &layer);

/**
 * The material's global paint channel set, as a bitmask over #eMaterialPaintChannel (`1 << channel`).
 *
 * When #Material::paint_layers_channels is zero the set is derived: the union of every layer's
 * channel records plus the build default (Base Color, Metallic, Roughness, Normal, AO), so a file
 * written before the field existed reads the same set it always offered. The derived set is never
 * empty. Base Color is an ordinary channel of the set and may be absent from a stored one; the
 * getter does not force it back in. An empty stored set is impossible: the setters refuse it.
 *
 * Derived and stored sets differ on purpose in one respect: a derived set (field zero) is a union
 * over the records, so it follows them when a record is added; a stored set is the user's filter and
 * never changes because a record was authored (#BKE_paint_layers_channel_add), only through the
 * setters. A caller that must put a channel into the set does so explicitly.
 * This is a read-only view of the description; it never creates a channel record.
 */
uint16_t BKE_paint_layers_channel_set_mask_get(const Material &ma);

/** Whether \a channel is in \a ma's global paint channel set. */
bool BKE_paint_layers_channel_in_set(const Material &ma, eMaterialPaintChannel channel);

/**
 * \a channel when \a ma's set holds it, otherwise the first channel of the set (in
 * #eMaterialPaintChannel order). For cursors that live outside the material, such as the Stack
 * Layers channel selection, which must not rest on a channel the material leaves out.
 */
int BKE_paint_layers_channel_clamp_to_set(const Material &ma, int channel);

/**
 * Add or remove \a channel from \a ma's global paint channel set, materializing the derived set on
 * the first explicit write (a zero field becomes the computed mask, then the edit is applied).
 *
 * The layers are never touched: no channel record or map is created or removed, so switching a
 * channel off keeps every layer's data and switching one on does not author records. Base Color is
 * an ordinary channel and may be switched off, but the set can never become empty: removing the
 * last channel is refused.
 *
 * \return false when \a channel is out of range, or when disabling it would leave the set empty.
 */
bool BKE_paint_layers_channel_set_enable(Material &ma,
                                         eMaterialPaintChannel channel,
                                         bool enabled);

/**
 * Write \a ma's global paint channel set as a whole bitmask. The mask cannot be empty (must have at
 * least one channel) and Base Color has no special status: it may be left out. The layers are never
 * touched; see #BKE_paint_layers_channel_set_enable.
 *
 * \return true when the mask was accepted (written, or already the stored one), false when \a mask
 * was empty and the set was left unchanged.
 */
bool BKE_paint_layers_channel_set_mask_set(Material &ma, uint16_t mask);

/**
 * The channel set a fresh stack offers before any set is authored: the build default (Base Color,
 * Metallic, Roughness, Normal, AO). The default #MATERIAL_OT_new_layered proposes in its dialog,
 * and what a zero #Material::paint_layers_channels field derives.
 */
uint16_t BKE_paint_layers_default_channel_set();

/**
 * Freeze the derived channel set into #Material::paint_layers_channels when the field is still zero.
 *
 * While the field is zero #BKE_paint_layers_channel_set_mask_get recomputes the union of every
 * layer record plus the build default on each call, which the per-channel filter does in hot loops.
 * This stores that derived view once, so the filter reads a plain bitmask afterwards. Quiet on
 * purpose -- the field is derived state, not a user edit -- so no tag, regeneration or undo step is
 * forced; callers on an authoring path already tag the material themselves.
 *
 * A no-op when the field is already nonzero, so it is idempotent and never touches a layer record.
 */
void BKE_paint_layers_channels_materialize(Material &ma);

/**
 * Remove \a layer from \a ma, freeing it and, for a folder, everything it holds.
 *
 * When the active marker names the removed row or a row nested under it, the active marker is
 * cleared: a marker that resolves to nothing is worse than no selection, and silently moving the
 * selection to a neighbour would hide that the row the user was working on is gone.
 *
 * \return false when \a layer is null or not part of \a ma.
 */
bool BKE_paint_layers_remove(Material &ma, MaterialPaintLayer *layer);

/**
 * The base mask item of \a layer (#MA_PAINT_LAYER_MASK_BASE), or null when the row has no mask.
 *
 * A mask is a coverage factor laid over its row: the row is wholly visible where the mask reads
 * one and hidden where it reads zero. The item is a #MA_PAINT_LAYER_ROLE_MASK_ITEM row that lives in
 * #MaterialPaintLayer::mask_stack, never in the stack itself; its Base Color record holds the
 * mask's constant until a stroke gives it a map. The channel, enabled and opacity edits below
 * accept the item like any row.
 */
MaterialPaintLayer *BKE_paint_layers_mask_base(MaterialPaintLayer &layer);
const MaterialPaintLayer *BKE_paint_layers_mask_base(const MaterialPaintLayer &layer);

/**
 * The constant coverage of the mask item \a item, in 0..1: the mean of its Base Color record, the
 * number the generator's value input and the CPU composite both read. One for an item with no
 * record. The UI and RNA go through this rather than reading the record's first component.
 */
float BKE_paint_layers_mask_constant(const MaterialPaintLayer &item);

/**
 * Give \a layer a base mask of constant \a value (clamped to 0..1): the row's coverage starts at
 * `F = value`, and a Mask-mode stroke turns the constant into a map.
 *
 * \return the new item, or null when \a layer is not part of \a ma or already has a mask.
 */
MaterialPaintLayer *BKE_paint_layers_mask_add(Material &ma, MaterialPaintLayer *layer, float value);

/**
 * Remove the whole mask of \a layer (the base item and any item over it), letting go of their maps.
 *
 * \return false when \a layer is not part of \a ma or has no mask.
 */
bool BKE_paint_layers_mask_remove(Material &ma, MaterialPaintLayer *layer);

/**
 * Give \a target a copy of \a source's mask, replacing the one it had. Every item of the copy has a
 * marker and maps of its own, so painting one mask never changes the other.
 *
 * \return false when \a source has no mask, the two are the same row, or \a target is not a row of
 * \a ma's stack.
 */
bool BKE_paint_layers_mask_copy(Main &bmain,
                                Material &ma,
                                const MaterialPaintLayer &source,
                                MaterialPaintLayer &target);

/**
 * Add a mask correction over \a layer's base mask: a #MA_PAINT_LAYER_ROLE_MASK_ITEM row with the
 * Mix blend and one Base Color record, linked on top of \a layer's mask. It lays `C` over the mask's
 * coverage `F` by `A * opacity`, `F = blend(F, C, A * opacity)`, with `C` the grey of its map (its
 * alpha `A`) or, as a Fill, its constant (`A` one).
 *
 * A Paint correction (the Image source) lays nothing until it has a map, which the first stroke in
 * Mask mode creates, transparent; a Fill correction (the Constant source) starts as the grey
 * \a value. Like a row's, its opacity, blend and visibility are its own settings.
 *
 * \param name: the correction's name, or null for "Mask Paint" / "Mask Fill".
 * \return the new item, or null when \a layer is not a row of \a ma's stack, has no base mask, or
 * \a source is neither Image nor Constant.
 */
MaterialPaintLayer *BKE_paint_layers_mask_item_add(Material &ma,
                                                   MaterialPaintLayer *layer,
                                                   eMaterialPaintLayerSource source,
                                                   const char *name,
                                                   float value = 1.0f);

/**
 * Remove the mask correction \a item from its row, letting go of its map. The base mask stays (it
 * goes with the whole mask, #BKE_paint_layers_mask_remove). The active marker moves to the row when
 * it named the item.
 *
 * \return false when \a item is the base or not a mask item of a row of \a ma.
 */
bool BKE_paint_layers_mask_item_remove(Material &ma, MaterialPaintLayer *item);

/** The row whose mask holds \a item (base or correction), or null when it is no mask item of
 * \a ma. */
MaterialPaintLayer *BKE_paint_layers_mask_item_owner(Material &ma, const MaterialPaintLayer &item);

/* -------------------------------------------------------------------- */
/** \name Color ID colours of a map-coverage mask item
 *
 * The colours a #MA_PAINT_LAYER_FILTER_COLOR_ID item matches. The list is owned by the item; these
 * calls are the only mutators RNA and the operators go through, so no caller writes the DNA list
 * directly. Adding or removing a colour changes the generated graph's shape (a comparison per
 * colour), so those tag a topology edit; the colour and its enabled flag are values the graph reads
 * as inputs, so those tag a value-only edit.
 * \{ */

/**
 * Add a colour to the Color ID item \a item, on top of the list (newest first), enabled.
 * \return the new record, or null when \a item is not a Color ID item of \a ma.
 */
MaterialPaintLayerIdColor *BKE_paint_layers_id_color_add(Material &ma,
                                                         MaterialPaintLayer *item,
                                                         const float color[3]);

/** Remove \a color from its item. False when it is not one of the item's colours. */
bool BKE_paint_layers_id_color_remove(Material &ma,
                                      MaterialPaintLayer *item,
                                      MaterialPaintLayerIdColor *color);

/** Enable or disable \a color; a disabled colour is skipped by the match. Value-only. */
bool BKE_paint_layers_id_color_set_enabled(Material &ma,
                                           MaterialPaintLayer *item,
                                           MaterialPaintLayerIdColor *color,
                                           bool enabled);

/** Set \a color's RGB, in the ID Map's raw data space. Value-only. */
bool BKE_paint_layers_id_color_set_color(Material &ma,
                                         MaterialPaintLayer *item,
                                         MaterialPaintLayerIdColor *color,
                                         const float value[3]);

/** Set the per-colour tolerance of \a color, used in Advanced mode. Value-only. */
bool BKE_paint_layers_id_color_set_tolerance(Material &ma,
                                             MaterialPaintLayer *item,
                                             MaterialPaintLayerIdColor *color,
                                             float tolerance);

/**
 * Make \a image read as data (the Non-Color role), as the ID Map of a Color ID item must: the
 * shader, the CPU mask and the picker then all see the file's raw texel values, whatever colorspace
 * the file was opened with. Does nothing when it already is data.
 */
void BKE_paint_layers_map_coverage_image_use_data(Main &bmain, Image &image);

/** Whether \a item is a map-driven mask coverage operator (Color ID or UV Island). */
bool BKE_paint_layers_is_map_coverage(const MaterialPaintLayer &item);

/**
 * The Image a map-coverage item samples, or null when it has none or its Base Color record is
 * disabled: the same rule the shader and the CPU mask follow.
 */
Image *BKE_paint_layers_map_coverage_image_get(const MaterialPaintLayer &item);

/** Tolerance and softness of the map-coverage item \a item (its kind's parameters). */
float BKE_paint_layers_coverage_tolerance_get(const MaterialPaintLayer &item);
float BKE_paint_layers_coverage_softness_get(const MaterialPaintLayer &item);

/** Whether the output of the mask item \a item is inverted (black where it matches). */
bool BKE_paint_layers_mask_item_is_inverted(const MaterialPaintLayer &item);

/** Whether the map-coverage item \a item outputs its match scaled by its gradient value. */
bool BKE_paint_layers_mask_item_is_gradient(const MaterialPaintLayer &item);

/**
 * Enable or disable the Gradient output of the map-coverage item \a item: its match is then scaled
 * by the item's Base Color constant (see #BKE_paint_layers_mask_item_output_value). Value-only.
 * \return false when \a item is not a map-coverage mask item of \a ma.
 */
bool BKE_paint_layers_mask_item_set_gradient(Material &ma,
                                             MaterialPaintLayer *item,
                                             bool gradient);

/** Whether the Color ID item compares each colour with its own tolerance (Advanced mode). */
bool BKE_paint_layers_mask_item_is_color_id_advanced(const MaterialPaintLayer &item);

/**
 * Enable or disable the per-colour tolerance mode of the Color ID item \a item. It changes the
 * generated graph's shape (one tolerance input per colour), so it is a topology edit.
 * \return false when \a item is not a Color ID mask item of \a ma.
 */
bool BKE_paint_layers_mask_item_set_color_id_advanced(Material &ma,
                                                      MaterialPaintLayer *item,
                                                      bool advanced);

/** Whether the output map of \a item carries its coverage in the alpha channel. */
bool BKE_paint_layers_mask_item_is_alpha_background(const MaterialPaintLayer &item);

/** Store the alpha-background choice of the map-coverage item \a item. Value-only. */
bool BKE_paint_layers_mask_item_set_alpha_background(Material &ma,
                                                     MaterialPaintLayer *item,
                                                     bool alpha_background);

/**
 * The scale the match of \a item is multiplied by: the item's Base Color constant in Gradient mode,
 * one otherwise. The generator, the CPU composite and the pickers read this one number.
 */
float BKE_paint_layers_mask_item_output_value(const MaterialPaintLayer &item);

/**
 * Whether the map of the map-coverage operator \a item may be written: the UV Island capture does,
 * a Color ID item's map is an external ID Map and is reference only. False for any other item.
 */
bool BKE_paint_layers_map_coverage_paintable(const MaterialPaintLayer &item);

/**
 * The Color ID item of \a ma whose list holds \a color, or null when it is in none (a stale
 * pointer). The colours carry no back pointer, so RNA finds the owner by address.
 */
MaterialPaintLayer *BKE_paint_layers_id_color_owner(Material &ma,
                                                    const MaterialPaintLayerIdColor &color);

/** \} */

/**
 * The mask correction the active marker names, or null when it names a row, a content correction or
 * nothing. A stroke in Mask mode paints it instead of the row's base mask.
 */
MaterialPaintLayer *BKE_paint_layers_active_mask_item_get(Material &ma);

/**
 * Add a content correction to \a owner: a #MA_PAINT_LAYER_ROLE_EFFECT row linked on top of \a owner's
 * #MaterialPaintLayer::effects.
 *
 * A Paint correction (the Image source) takes the material's channel records, transparent and with
 * no map yet -- the first stroke creates one -- so it lays down nothing until painted. A Fill
 * correction (the Constant source) is one flat colour: a Base Color record, opaque black until
 * #BKE_paint_layers_set_fill_color gives it a colour, which it lays at its own opacity over the
 * row's colour. Another channel is switched on with #BKE_paint_layers_channel_add and starts at that
 * colour; a channel that holds a map reads the map instead, like a Paint correction's, and a Fill
 * lays nothing in Normal while it has none, where a flat value is not a normal.
 *
 * \param name: the correction's name, or null for "Correction Paint" / "Correction Fill".
 * \param source: #MA_PAINT_LAYER_SOURCE_IMAGE or #MA_PAINT_LAYER_SOURCE_CONSTANT.
 * \return the new correction, or null when \a owner is not a row of \a ma's stack or \a source is
 * neither. A folder takes one: it corrects the isolated result of what the folder holds, before the
 * folder's own blend, and a folder that has one is no longer a Pass Through folder.
 */
MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    const char *name,
                                                    eMaterialPaintLayerSource source);

/** As above for a Paint correction (the Image source). */
MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    const char *name);

/**
 * Remove the content correction \a effect from its owner, letting go of its maps. The active
 * marker is cleared when it names the correction.
 *
 * \return false when \a effect is null or not a correction of a row of \a ma.
 */
bool BKE_paint_layers_correction_remove(Material &ma, MaterialPaintLayer *effect);

/** The row that holds \a effect in its #MaterialPaintLayer::effects, or null when it is not a
 * correction of a row of \a ma. */
MaterialPaintLayer *BKE_paint_layers_correction_owner(Material &ma, const MaterialPaintLayer &effect);

/**
 * Whether #BKE_paint_layers_correction_paste can attach a copy of the content correction or mask
 * item \a source to \a target: \a target must be a stack row (a folder included), and a mask item
 * that is not a base needs a base on \a target to sit over.
 */
bool BKE_paint_layers_correction_can_paste(const MaterialPaintLayer &source,
                                           const MaterialPaintLayer &target);

/**
 * Attach a deep copy of the content correction or mask item \a source -- which may belong to
 * another material -- to \a target: a content correction goes to the top of its corrections, a
 * mask item to the top of its mask. The copy gets a marker unique within \a ma and copies of the
 * images its records hold (never the shared image), and keeps its settings, its flat colour and its
 * inversion. A base mask lands as the target's base when it has none, and as a plain item over its
 * own otherwise.
 *
 * \return the copy, or null when \a target is not a row of \a ma or
 * #BKE_paint_layers_correction_can_paste refuses.
 */
MaterialPaintLayer *BKE_paint_layers_correction_paste(Main &bmain,
                                                      Material &ma,
                                                      const MaterialPaintLayer &source,
                                                      MaterialPaintLayer *target);

/**
 * Find the row of \a ma whose marker is \a marker, at any depth and never by position, so a row
 * keeps resolving after a reorder.
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
 * names no row. A content correction is answered as itself; a mask correction as the row it hangs
 * on, whose mask is what the mode paints (the correction is #BKE_paint_layers_active_mask_item_get).
 */
MaterialPaintLayer *BKE_paint_layers_active_layer_get(Material &ma);

/**
 * Move \a layer relative to \a anchor in \a ma's stack.
 *
 * Same placement rules as #BKE_paint_layers_add: a null anchor appends the row on top of the stack,
 * Above and Below are siblings of the anchor, Into appends inside the anchor, which has to be a
 * folder. Markers are untouched, so the active marker keeps resolving.
 *
 * \return false when \a layer or \a anchor is not part of \a ma, when \a anchor is \a layer itself
 * or nested under it (a row never moves into its own subtree), or when Into names a plain row.
 */
bool BKE_paint_layers_move(Material &ma,
                           MaterialPaintLayer *layer,
                           MaterialPaintLayer *anchor,
                           PaintLayerPlace place);

/**
 * Move \a layer to \a index among its siblings, bottom to top: its position in the list that holds
 * it, never its parent. The index is clamped to that list.
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
 * \return the record, or null when \a layer is null or not part of \a ma, or is a folder or a
 * Material row: those take part through what they hold or read, not through records of their own.
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

/**
 * Set whether \a layer's \a channel record is read as `1 - C`. Only a mask correction's Base Color
 * record honours it (see #MA_PAINT_LAYER_CHANNEL_INVERT); the flag is kept on any other record
 * without effect. Part of the graph's structure, so it regenerates.
 *
 * \return false when \a layer carries no \a channel record or is not part of \a ma.
 */
bool BKE_paint_layers_channel_set_invert(Material &ma,
                                         MaterialPaintLayer *layer,
                                         eMaterialPaintChannel channel,
                                         bool invert);

/**
 * Set whether \a layer's Normal record reads its map as a DirectX tangent-space normal map
 * (#MA_PAINT_LAYER_CHANNEL_NORMAL_DIRECTX) rather than OpenGL. The generator flips the map's green
 * channel with nodes, so the choice is visible in the Shader Editor. Part of the graph's
 * structure, so it regenerates.
 *
 * \return false when \a layer carries no Normal record or is not part of \a ma.
 */
bool BKE_paint_layers_normal_set_directx(Material &ma, MaterialPaintLayer *layer, bool directx);

/**
 * Set the per-channel opacity multiplier of \a layer's \a channel, applied over the row's own
 * opacity. Any row may carry one, a folder or a correction included, whether or not it has a
 * record for the channel.
 *
 * Value-only: the generated tree's per (row, channel) opacity input is brought current through
 * #BKE_paint_layers_values_sync, so the edit never rebuilds the topology.
 *
 * \return false when \a layer is not part of \a ma or \a channel is out of range.
 */
bool BKE_paint_layers_channel_opacity_set(Material &ma,
                                          MaterialPaintLayer &layer,
                                          int channel,
                                          float opacity);

/**
 * Set the blend \a layer's \a channel is laid with, or `-1` to inherit the row's own. Any row may
 * carry one, a folder or a correction included, whether or not it has a record for the channel.
 *
 * The Normal channel has its own two choices: `-1` for the combine and
 * #MA_PAINT_LAYER_BLEND_NORMAL_REPLACE for Replace; every other mode is refused there. On the
 * other channels #MA_PAINT_LAYER_BLEND_NORMAL_COMBINE is refused, as it is for the row.
 *
 * A structural edit: the blend is the type of the generated Mix node, so the tree is marked stale.
 *
 * \return false when \a layer is not part of \a ma or \a channel is out of range.
 */
bool BKE_paint_layers_channel_blend_set(Material &ma,
                                        MaterialPaintLayer &layer,
                                        int channel,
                                        int blend);

/**
 * The next time, in `BLI_time_now_seconds()` units, at which a scrubbing blend channel is due to
 * be simplified back to a static Mix node, or -1 when nothing is waiting.
 *
 * The tier state itself lives in BKE; the editor's timer only asks when to wake up.
 */
double BKE_paint_layers_blend_tier_next_deadline();

/**
 * Drop every scrubbing blend record whose deadline has passed, and mark the materials that owned
 * one for a full regeneration, so their groups go back to static Mix nodes. Returns whether
 * anything changed. The editor's timer calls this from the main thread.
 */
bool BKE_paint_layers_blend_tier_demote_due(Main &bmain, double now);

/** Forget all runtime blend-tier records. Used when a file is loaded, undone or closed. */
void BKE_paint_layers_blend_tier_clear();

/**
 * Reconcile a freshly loaded or restored file with the runtime-only tier: forget every record, and
 * mark each layered material whose generated tree says it was saved with the dynamic group for a
 * full regeneration. The marker is the only persisted trace; after this the tree is static again.
 */
void BKE_paint_layers_blend_load_post(Main &bmain);
void BKE_paint_layers_blend_undo_post(Main &bmain);

/** Set the opacity of \a layer. */
bool BKE_paint_layers_set_opacity(Material &ma, MaterialPaintLayer *layer, float opacity);

/** Set whether \a layer takes part in the stack. */
bool BKE_paint_layers_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled);

/**
 * Set the display color tag of the folder \a layer; #MA_PAINT_LAYER_COLOR_NONE clears it.
 * Interpreted by the UI only, so nothing evaluated is tagged. A tag on a row that is not a folder
 * is refused: only a group can be colored. Out-of-range values are refused too.
 */
bool BKE_paint_layers_set_color_tag(Material &ma, MaterialPaintLayer *layer, int color_tag);

/* -------------------------------------------------------------------- */
/** \name Row UV mapping (a Fill layer, Fill correction or Fill mask)
 * \{ */

/** Lower bound of the mapping scale magnitude; a zero axis reads back as one. */
constexpr float PAINT_LAYER_MAPPING_SCALE_MIN = 1e-3f;

/**
 * Normalized mapping scale of one axis: zero reads back as one, the magnitude never drops below
 * #PAINT_LAYER_MAPPING_SCALE_MIN. The one helper RNA, value sync and the CPU compositor share, so
 * an old file's zeroes cannot disagree between them.
 */
float BKE_paint_layers_mapping_scale_normalize(float scale);

/**
 * Whether a Fill row reads a user-assigned map in \a channel instead of its constant: a Layer or a
 * correction of a kind that lays a flat value, whose live channel record carries an image. The CPU
 * composite and the mapping predicates all ask this, so they cannot disagree about it.
 */
bool BKE_paint_layers_fill_reads_map(const Material &ma,
                                     const MaterialPaintLayer &row,
                                     int channel);

/**
 * Whether \a layer may carry UV mapping: a Fill that reads a map in at least one channel (Layer,
 * correction) or through its mask image (mask item), or a Material row whose source reads the
 * object's active UVs in its root tree or in a group on the path to its Principled BSDF
 * (#BKE_paint_layers_source_reads_uv). A Material row moves the UV at which its source is read --
 * its live textures, its rendered channel maps and its source-group wrapper. A map written by
 * strokes (Paint) is addressed at the raw UV and never supports it; neither does a filter, which
 * owns no map. The one predicate UI, RNA, the generator and the CPU share.
 */
bool BKE_paint_layers_mapping_supported(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether \a layer's mapping is applied right now: enabled and supported. The values never move
 * this answer; a slider drag must not rebuild anything.
 */
bool BKE_paint_layers_mapping_applies(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether strokes may write into \a row's own channels. A Fill row lays a flat value (with an
 * optional map read through its mapping); painting it would turn that map into a stroke target.
 * Its content is edited by corrections instead. A row's mask is still paintable.
 */
bool BKE_paint_layers_row_is_paintable(const MaterialPaintLayer &row);

/** Whether \a layer's mapping is enabled. */
bool BKE_paint_layers_mapping_enabled_get(const MaterialPaintLayer &layer);

/**
 * Set whether \a layer's mapping is enabled. Refused when #BKE_paint_layers_mapping_supported
 * reports false. Structural: the row gains or loses its remap.
 */
bool BKE_paint_layers_mapping_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled);

/** Set the mapping offset of \a layer. Value-only. */
bool BKE_paint_layers_mapping_set_offset(Material &ma,
                                         MaterialPaintLayer *layer,
                                         const float offset[2]);

/**
 * Set the mapping scale of \a layer. Value-only, normalized through
 * #BKE_paint_layers_mapping_scale_normalize.
 */
bool BKE_paint_layers_mapping_set_scale(Material &ma,
                                        MaterialPaintLayer *layer,
                                        const float scale[2]);

/**
 * Set offset, scale and rotation of \a layer in one step (one tag per call, for per-event gizmo
 * drags). Value-only. While the scale lock is on both axes take the X value of \a scale.
 */
bool BKE_paint_layers_mapping_set_values(Material &ma,
                                         MaterialPaintLayer *layer,
                                         const float offset[2],
                                         const float scale[2],
                                         float rotation);

/** Whether the scale axes of \a layer move together (the Lock toggle). */
bool BKE_paint_layers_mapping_scale_lock_get(const MaterialPaintLayer &layer);

/**
 * Lock or unlock the scale axes of \a layer. While locked, #BKE_paint_layers_mapping_set_scale
 * gives both axes the value of the one that changed; locking joins them at the X value at once.
 * Value-only.
 */
bool BKE_paint_layers_mapping_set_scale_lock(Material &ma, MaterialPaintLayer *layer, bool locked);

/**
 * Set the mapping rotation of \a layer, in radians around Z. Value-only.
 *
 * The rotation changes where the row's maps are read. A Normal map's tangent-space vectors are
 * rotated by it too (and flipped by the sign of the scale), so the lighting follows the pattern.
 */
bool BKE_paint_layers_mapping_set_rotation(Material &ma, MaterialPaintLayer *layer, float rotation);

/**
 * Which axes of a UV \a wrap repeat: \a r_repeat_u / \a r_repeat_v are true for an axis that tiles
 * and false for one that clips (transparent outside [0, 1]). The one reading of the enum, shared by
 * the CPU resample and the generator.
 */
void BKE_paint_layers_mapping_wrap_axes(int16_t wrap, bool &r_repeat_u, bool &r_repeat_v);

/**
 * The UV wrap of \a layer. A stored value outside the enum reads as Repeat, so a damaged field
 * never clips a map. Applied by the graph and the CPU only while the row's mapping applies.
 */
int16_t BKE_paint_layers_mapping_wrap_get(const MaterialPaintLayer &layer);

/**
 * Set the UV wrap of \a layer. Structural: Repeat builds nothing extra, any other mode adds the
 * clip factor's nodes. False for a value outside #eMaterialPaintLayerMappingWrap or a row not in
 * \a ma.
 */
bool BKE_paint_layers_mapping_set_wrap(Material &ma, MaterialPaintLayer *layer, int16_t wrap);

/** \} */

/** Rename \a layer. */
bool BKE_paint_layers_rename(Material &ma, MaterialPaintLayer *layer, const char *name);

/**
 * Set how \a layer combines with what is below it; see #eMaterialPaintLayerBlend.
 *
 * #MA_PAINT_LAYER_BLEND_NORMAL_COMBINE and #MA_PAINT_LAYER_BLEND_NORMAL_REPLACE are refused: the
 * Normal channel forces its operation on its own, so a stored row value would only ever be a
 * switch the user cannot make effective.
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
 * Whether \a from reads \a target, directly or through the sources of its own Material rows (the
 * rows of the stack, their corrections and masks, and what folders hold). A material reads itself.
 * The walk follows the descriptions only: it asks nothing of the generated trees.
 */
bool BKE_paint_layers_material_depends_on(const Material &from, const Material &target);

/** Why a Material row of \a target cannot read \a source, if it cannot. */
enum class PaintLayerMaterialSourceIssue : int8_t {
  /** The source may be read. */
  None,
  /** The row would read the material that owns it. */
  Self,
  /** The source already reads the owner, so the two would read each other. */
  Cycle,
};

/**
 * The refusal #BKE_paint_layers_add_with_policy and #BKE_paint_layers_set_material share, named so
 * a UI can explain the refusal without restating the rule. The walk is the one
 * #BKE_paint_layers_material_depends_on makes.
 */
PaintLayerMaterialSourceIssue BKE_paint_layers_material_source_issue(const Material &target,
                                                                     const Material &source);

/** Why a row cannot take a dropped image as one of its maps, if it cannot. */
enum class PaintLayerImageMapIssue : int8_t {
  /** The row takes the image as a map of a channel. */
  None,
  /** A painted row's maps belong to its brush and are not replaced from outside. */
  PaintRowOwnsMaps,
  /** Only a Fill row, or a Fill correction, shows the user's own maps. */
  TakesNoImage,
};

/**
 * Whether \a row takes an image dropped onto it: a Fill row does, a painted row does not, and a
 * row that composites or shows another material has no map of its own to take. Named so the drop
 * and the popup it opens judge a row the same way.
 */
PaintLayerImageMapIssue BKE_paint_layers_row_image_map_issue(const MaterialPaintLayer &row);

/**
 * Point the Material row \a layer at \a source, taking the user of the new material and letting go
 * of the old one. Refused (false) for a row that is not a Material row, for a null \a source, for
 * the owner itself and for a source that already reads \a ma: the pair would read each other, and
 * every walk over the sources would never end.
 */
bool BKE_paint_layers_set_material(Material &ma, MaterialPaintLayer *layer, Material *source);

/**
 * Whether the material \a layer reads is a directly linked source that
 * #BKE_paint_layers_material_source_make_local can take: \a layer is a Material row of an editable,
 * non-override \a ma. A source that is merely an indirect dependency is not the user's to take.
 */
bool BKE_paint_layers_material_source_can_make_local(const Material &ma,
                                                     const MaterialPaintLayer *layer);

/**
 * Make the material \a layer reads local to this file, so its nodes can be edited in place.
 *
 * A Material row can read a material that lives in another library, which the properties editor
 * shows but cannot edit. Localizing it either strips the library from the ID in place or copies it
 * and remaps its users -- this row included -- so \a layer is the authority on which material it
 * now reads. Only a source #BKE_paint_layers_material_source_can_make_local accepts is taken, the
 * same rule the Outliner's Localize uses.
 *
 * \return the local material the row now reads, or null when \a layer reads no such material or the
 *         source could not be made local.
 */
Material *BKE_paint_layers_material_source_make_local(Main &bmain,
                                                      Material &ma,
                                                      MaterialPaintLayer *layer);

/* -------------------------------------------------------------------- */
/** \name Material rows: maps rendered from the source
 *
 * A Material row shows maps the editor renders from its source material (one per channel of the
 * owner's set, and the source's transparency as the row's coverage). They live in the row's bake
 * (#MaterialPaintLayer::bake), but are not a cache of the row's subtree: the row's opacity, blend,
 * mask and corrections stay live over them. The row's bake hash is only structural (the source, the
 * size, the channel set); whether the maps still describe the source is the source's content hash
 * and the pixel subscription, read on the main thread. While they do not, the old maps keep showing
 * until the new ones land.
 * \{ */

/**
 * The content hash of \a ma's node trees (topology plus every value that changes what they
 * evaluate to), or zero when it has none. Walks every node, so the main thread asks for the cached
 * answer below.
 */
uint64_t BKE_paint_layers_source_material_tree_hash(const Material &ma);

/**
 * As above, remembered against the cheap node tree update signal so a scene update that changed
 * nothing of the source does not walk it. Main thread only.
 */
uint64_t BKE_paint_layers_source_material_tree_hash_cached(const Material &ma);

/**
 * As #BKE_paint_layers_source_material_tree_hash but without the default values of the sockets
 * (nodes' inputs and outputs, group interfaces): what stays the same while a value is dragged.
 * Equal structure hashes with different content hashes mean that only socket values moved.
 */
uint64_t BKE_paint_layers_source_material_structure_hash(const Material &ma);

/** The structure hash, remembered against the node tree update signal. Main thread only. */
uint64_t BKE_paint_layers_source_structure_hash_cached(const Material &ma);

/**
 * The content hash of what a rendered Normal depends on in \a ma: the graph upstream of the
 * Principled BSDF's Normal and of the Material Output's Displacement. Remembered as the hash above.
 * Main thread only.
 */
uint64_t BKE_paint_layers_source_normal_hash_cached(const Material &ma);

/**
 * What \a source supplies to every paint channel (#BKE_paint_material_source_resolve), remembered
 * against the node tree update signal so the generator and the CPU composite, which ask once per
 * row and channel, do not walk the source each time. The answer points into \a source's own node
 * trees and is only good for as long as they are not edited; it is made from the tree it is asked
 * of, so an evaluated copy never gets the original's pointers.
 */
std::shared_ptr<const MaterialSourceResolve> BKE_paint_layers_source_resolve(
    const Material &source);

/**
 * Whether the Principled BSDF that drives \a source's Material Output is reached through no more
 * than #SOURCE_GROUP_MAX_DEPTH nested groups: the condition for copying the source's graph into a
 * group of its own. Remembered with the resolve above.
 */
bool BKE_paint_layers_source_group_wrappable(const Material &source);

/**
 * Whether \a source's shader graph reads the object's active UVs (a Texture Coordinate, an unnamed
 * UV Map, or an Image Texture with an open Vector) in the root tree or in a group on the path to
 * its Principled BSDF, the levels the source group copies and maps. A reader inside a group beside
 * that path is shared as is and not counted. A Material row with no such reader has nothing for
 * its UV mapping to move. Memoized with the source's resolve, so it is cheap to ask on a redraw.
 */
bool BKE_paint_layers_source_reads_uv(const Material &source);

/** The most nested groups a source's graph is followed through to its Principled BSDF. */
inline constexpr int SOURCE_GROUP_MAX_DEPTH = 8;

/** Drop the remembered hash and resolve of \a ma, which is being freed. */
void BKE_paint_layers_source_material_hash_forget(const Material &ma);

/**
 * Whether the colour \a channel (or, for the alpha channel, the coverage) of the Material row
 * \a row has to be rendered into a map: its source supplies it as a graph rather than as a plain
 * value or a plain Image Texture, which are shown as they are. A channel outside the material's
 * set needs nothing.
 */
bool BKE_paint_layers_material_channel_needs_render(const Material &ma,
                                                    const MaterialPaintLayer &row,
                                                    int channel);

/**
 * Whether the render that is about to start for \a row should make the map of \a channel: it needs
 * one, and the shader does not already show the channel live while nothing reads the map. A row
 * whose only channel without a live form is the Normal renders just that, the cost of an edit of a
 * Bump being one channel and not all of them. Called wherever the render's channels are chosen,
 * with the state the stamp (#BKE_paint_layers_material_bake_stamp_get) was made in.
 */
bool BKE_paint_layers_material_channel_render_wanted(const Material &ma,
                                                     const MaterialPaintLayer &row,
                                                     int channel);

/** Whether any channel of \a row needs a map (#BKE_paint_layers_material_channel_needs_render). */
bool BKE_paint_layers_material_row_needs_render(const Material &ma, const MaterialPaintLayer &row);

/**
 * Whether the maps of the Material row \a layer no longer describe its source: it has none yet, the
 * source's content moved since the render started, or a texture the source reads was painted on.
 * Main thread only.
 */
bool BKE_paint_layers_material_row_stale(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether the editor should render the maps of the Material row \a layer now: it reads a material
 * that has a node tree, is not set to Never, is not hidden, its maps are missing or stale, and the
 * last render started from this very state did not just fail. Main thread only.
 *
 * The row being edited (the active one) is only due while its maps are wanted as a preview
 * (#BKE_paint_layers_material_row_preview_active); a row left behind with maps of preview quality
 * is due for the render at its own size.
 */
bool BKE_paint_layers_material_row_due(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether the render of the Material row \a layer now is a preview: the row is being edited, its
 * graph is shown live in the shader (not Always, whose maps are what the row shows), and an Image
 * Editor shows the material's composite so its maps are wanted.
 */
bool BKE_paint_layers_material_row_preview_active(const Material &ma,
                                                  const MaterialPaintLayer &layer);

/** Whether the maps of the Material row \a layer are of preview quality (see #MaterialPaintLayerBake). */
bool BKE_paint_layers_material_row_is_preview(const MaterialPaintLayer &layer);

/**
 * The side the maps of the Material row \a layer are rendered at when it is not being edited: its
 * own size, or the size the material's layers are created with while it has none (4096 by default).
 */
int BKE_paint_layers_material_row_size(const Material &ma, const MaterialPaintLayer &layer);

/**
 * The side a preview of a row of \a final_size renders at: the material's preview setting, never
 * above \a final_size or the size layers of the material are created with, 16 pixels at the least.
 */
int BKE_paint_layers_preview_size_get(const Material &ma, int final_size);

/**
 * How long after the last edit of \a source the maps of the row that reads it are rendered when
 * \a consumer is edited through its active row, in seconds: the material's delay, or zero when it
 * syncs immediately or the edit is not made through the active row (the editor's short pause
 * applies).
 */
float BKE_paint_layers_source_edit_delay(const Material &consumer, const Material &source);

/** Whether an Image Editor shows the composite of \a ma, as the editor last found at an update. */
bool BKE_paint_layers_preview_wanted(const Material &ma);
void BKE_paint_layers_preview_wanted_set(Material &ma, bool wanted);

/** Print the state that decides whether the render of the Material row \a layer is due. */
void BKE_paint_layers_material_row_log(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const char *event);

/**
 * The hashes a render of \a layer starts from: the structural one and the source's content hash,
 * low word first. Handed back to #BKE_paint_layers_material_bake_land when the render ends.
 */
void BKE_paint_layers_material_bake_stamp_get(const Material &ma,
                                              const MaterialPaintLayer &layer,
                                              uint32_t r_hash[2],
                                              uint32_t r_source_hash[2]);

/**
 * Hand the maps of a render about to start to the Material row \a layer: one image per entry of
 * \a channels (an alpha entry becomes the row's coverage), every other channel's map is let go, a
 * coverage the render does not hand a map for is let go too. The structural hash is stamped and
 * the pixel subscription started now, so the generator wires the new maps in while they render and
 * an edit made during the render is seen after it. The source hash waits for
 * #BKE_paint_layers_material_bake_land.
 */
void BKE_paint_layers_material_bake_apply(Main &bmain,
                                          Material &ma,
                                          MaterialPaintLayer &layer,
                                          int size,
                                          Span<int> channels,
                                          Span<Image *> images);

/**
 * A render for the Material row \a marker ended, started from \a hash / \a source_hash. Landed,
 * the row takes \a source_hash as what its pixels reflect (unless the row itself changed
 * meanwhile, and then they are not its own). Not landed (cancelled, failed), the state is
 * remembered so the same render is not started again by every scene update.
 *
 * \a preview_side is the side the render was a preview at, or zero for a render at the row's own
 * size: the maps of the row are of preview quality until one at its own size lands.
 *
 * \return whether the row took the maps.
 */
bool BKE_paint_layers_material_bake_land(Material &ma,
                                         const bUUID &marker,
                                         const uint32_t hash[2],
                                         const uint32_t source_hash[2],
                                         bool landed,
                                         int preview_side = 0);

/** Forget that the last render of \a layer failed and mark its maps stale: it is rendered again. */
void BKE_paint_layers_material_bake_retry(Material &ma, MaterialPaintLayer &layer);

/** \} */

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

/**
 * Add a filter correction to \a owner (a layer or a folder): it changes the colour the row has
 * built so far in every channel the row takes part in, apart from Normal, and owns no map.
 * \return The new correction, or null when \a owner is not in the stack or \a filter is unknown.
 */
MaterialPaintLayer *BKE_paint_layers_filter_add(Material &ma,
                                                MaterialPaintLayer *owner,
                                                eMaterialPaintLayerFilter filter,
                                                const char *name = nullptr);

/**
 * Add a filter to the mask of \a layer, over the items already there: it changes the coverage built
 * so far (`M = lerp(M, f(M), opacity)`), owns no map or grey, and works in no channel. The row needs
 * a base mask first, as for any mask correction.
 * \return The new item, or null when \a layer has no base mask, is not in the stack, or \a filter is
 * unknown.
 */
MaterialPaintLayer *BKE_paint_layers_mask_filter_add(Material &ma,
                                                     MaterialPaintLayer *layer,
                                                     eMaterialPaintLayerFilter filter,
                                                     const char *name = nullptr);

/** Why a row of a layered material behaves differently than its settings suggest. */
enum class PaintLayersIssueCode : int8_t {
  /**
   * A Fill correction that carries the Normal channel while it holds no map in it. A flat value is
   * not a tangent normal, so the generator and the CPU composite both skip the correction in that
   * channel.
   */
  FillCorrectionOnNormal = 0,
  /**
   * A folder carrying a map of its own. A folder takes part in a channel through its children (and
   * its corrections), so a map on the folder itself is ignored; a record without a map is fine, it
   * carries the pair's settings.
   */
  FolderHasMaps = 1,
  /**
   * A row that is not a folder yet holds children. Only a folder takes part through its children,
   * so the stray nesting is ignored; this catches a malformed file or an edit made around the API.
   */
  NonFolderHasChildren = 2,
  /**
   * A row whose kind this build does not know, or that sits in a role its kind does not allow (a
   * folder as a correction). Such a row is skipped; this catches a file written by a build with
   * more kinds, or an edit made around the API.
   */
  KindNotAllowed = 3,
  /**
   * An enabled map-coverage mask item (Color ID, UV Island) without a map. The operator still takes
   * part and matches nothing (`match = 0`), so a whole mask can disappear with no visible reason;
   * this says why.
   */
  MapCoverageMissingMap = 4,
  /**
   * An enabled Fill row or Fill correction while Base Color is outside the material's channel set.
   * A Fill's colour lives in its Base Color record, and a channel outside the set takes part
   * nowhere, so the row's flat colour has nowhere to go. This says why instead of leaving a silent
   * no-op; the row may still take part through other channels that carry maps or values.
   */
  FillWithoutBaseColor = 5,
};

/** One entry #BKE_paint_layers_issues_get reports. */
struct PaintLayersIssue {
  /** The layer the issue is about. */
  bUUID layer = {};
  /** The correction the issue is about, or nil when it is about the layer itself. */
  bUUID correction = {};
  /** The channel the issue is about, an #eMaterialPaintChannel. */
  int channel = 0;
  PaintLayersIssueCode code = PaintLayersIssueCode::FillCorrectionOnNormal;
  /** A human-readable description. Static storage; never freed by the caller. */
  const char *text = nullptr;
};

/**
 * Collect the description problems that would otherwise be silent: a row whose settings cannot do
 * anything where the user put it.
 *
 * The generator and the CPU composite skip such a row (they always agree on which), and this list
 * is how a UI is meant to say why. It is computed from the description alone, never from a graph,
 * so it is cheap enough to call wherever a row is drawn.
 */
void BKE_paint_layers_issues_get(const Material &ma, Vector<PaintLayersIssue> &r_issues);

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
 * #BKE_paint_layers_effective_opacity scaled by  channel's own opacity; the row's alone when it
 * has no record for  channel. What every evaluator uses for a channel's stack.
 */
float BKE_paint_layers_channel_opacity_effective(const MaterialPaintLayer &layer, int channel);

/**
 * The blend \a layer is laid with in \a channel: the channel's own override, or the row's blend
 * while it inherits. What every evaluator uses for a channel's stack.
 */
int BKE_paint_layers_channel_blend_effective(const MaterialPaintLayer &layer, int channel);

/**
 * Whether the row's Normal channel is set to Replace: the upper normal covers the lower one with a
 * plain Mix instead of the Normal Combine.
 */
bool BKE_paint_layers_normal_replace(const MaterialPaintLayer &layer);

/**
 * The value \a channel's stack starts from, before any row is laid over it: the shader-space RGBA
 * a CPU composite must initialise its buffer to.
 *
 * The values are the channel's neutral default -- what the Principled BSDF shows where nothing
 * covers it -- and the alpha is used only where a channel has one. Every evaluator reads this one
 * table, or a partially covered row would fade towards different values in each of them.
 */
void BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel channel, float r_color[4]);

/**
 * Append every image a layered material of \a bmain holds a map in -- a row's, a correction's or a
 * mask item's -- to \a r_images, once each.
 */
void BKE_paint_layers_maps_collect(const Main &bmain, Vector<Image *> &r_images);

/**
 * Whether #BKE_paint_layers_pack_maps takes care of \a image on a file write: an editable
 * generated map, or one that is already packed. Any other map (an external file the user assigned)
 * keeps its pixels in that file and has to go through the usual save prompt.
 */
bool BKE_paint_layers_map_is_packed_on_save(const Image &image);

/**
 * Pack the maps of every layered material of \a bmain, so a written file reopens with its pixels.
 *
 * A map is a generated image: its pixels exist only in the buffer, and the file keeps nothing but
 * the generation settings (an empty map) unless the image is packed. Packing turns it into a
 * packed file-source image; a map already packed and not modified since is left alone, so the
 * cost follows what changed, not what exists.
 *
 * \param for_autosave: write into the images' auto-save packed files instead, which leaves the
 * session's images as they are (auto-save and session recovery); only generated maps the image
 * editor's own auto-save flush did not cover are handled.
 * \return The number of maps packed. A map that could not be packed is reported to \a reports.
 */
int BKE_paint_layers_pack_maps(const Main &bmain, bool for_autosave, ReportList *reports);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake
 *
 * A baked row is a cache of its live subtree: the description plus its live maps stay what the row
 * is, and the bake is read instead of them only while it is valid. The planner re-bakes a row whose
 * stored hash no longer matches, synchronously for a light one and on a worker for a heavy one.
 * \{ */

/** The bake mode of \a layer, an #eMaterialPaintLayerBakeMode (AUTO when nothing is baked). */
int BKE_paint_layers_bake_mode_get(const MaterialPaintLayer &layer);

/** Set the bake mode of \a layer, allocating its bake cache if needed. */
bool BKE_paint_layers_bake_mode_set(Material &ma, MaterialPaintLayer &layer, int mode);

/** The square side \a layer's maps are baked at, or 0 when nothing is baked. */
int BKE_paint_layers_bake_size_get(const MaterialPaintLayer &layer);

/** Set the square side \a layer's maps are baked at, invalidating any stored bake. */
bool BKE_paint_layers_bake_size_set(Material &ma, MaterialPaintLayer &layer, int size);

/** Mark \a layer's bake stale so the planner re-bakes it; allocates the cache if needed. */
void BKE_paint_layers_bake_request(MaterialPaintLayer &layer);

/** Drop \a layer's bake cache and mark the generated tree stale. */
bool BKE_paint_layers_bake_clear(Material &ma, MaterialPaintLayer &layer);

/** Whether a value edit or a pixel change left some row of \a ma waiting to be re-baked. */
bool BKE_paint_layers_bake_stale_get(const Material &ma);
/** Clear \a ma's pending-bake mark; the planner calls this once the queue is drained. */
void BKE_paint_layers_bake_stale_clear(Material &ma);

/**
 * Whether the editor has a bake outstanding for \a ma right now: a debounce timer armed after an
 * edit, or a bake job started and not finished. BKE cannot see `wmJob` state, so this is the
 * editor's hand-off, set and cleared by the editor's scheduling code alone, never derived here.
 */
bool BKE_paint_layers_bake_scheduled_get(const Material &ma);
/** Set or clear \a ma's outstanding-bake mark; see #BKE_paint_layers_bake_scheduled_get. */
void BKE_paint_layers_bake_scheduled_set(Material &ma, bool scheduled);

/**
 * Append every layered material of \a bmain that has a Material row reading \a source, once each.
 *
 * The render of a row is keyed on the row, not on its source, so an edit of the source reaches the
 * editor as an edit of \a source alone; this is the list of materials whose renders it concerns.
 * Localized and evaluated copies are skipped: they share the original's description.
 */
void BKE_paint_layers_source_material_consumers(Main &bmain,
                                                const Material &source,
                                                Vector<Material *> &r_consumers);

/**
 * Whether \a ma has a render nobody has started yet: a heavy row without a valid bake
 * (#BKE_paint_layers_bake_heavy_pending) or a Material row whose maps are due
 * (#BKE_paint_layers_material_row_due).
 */
bool BKE_paint_layers_bake_outstanding(const Material &ma);

/**
 * Whether \a ma's baked result (the maps the stack reads, the Image Editor's composite, exported
 * passes) is not the one its current description would produce: a pending value or pixel edit, a
 * render nobody has started (#BKE_paint_layers_bake_outstanding), or the editor's own mark of a
 * timer armed or a job running (#BKE_paint_layers_bake_scheduled_get).
 *
 * This is the BKE half of "is the result fresh": it cannot see a job the editor has not yet
 * marked, so a caller that has a window manager should ask `ED_paint_layers_stale_or_pending`.
 */
bool BKE_paint_layers_is_stale(const Material &ma);

/** What a caller that wants a baked result should do about a stale one. */
enum class PaintLayersBakeGateAction {
  /** Nothing is stale, or nothing can be done about it here (headless): go ahead now. */
  Proceed,
  /** `invoke` reached a stale result: wait for the bake (go modal) and ask again. */
  Wait,
  /** `exec` or an RNA function reached a stale result and cannot wait: report and stop. */
  Refuse,
};

/**
 * The one rule every consumer of a baked result shares, from three things it knows about itself:
 * whether the result it read is \a stale, whether it was reached through `invoke` (a person, who
 * can wait a moment) rather than `exec` or an RNA function (a script, which cannot), and whether
 * it runs \a headless (no window manager). Headless always proceeds: there is no timer, no job and
 * no modal loop that would ever make the result fresh, so refusing it would only be a false
 * refusal.
 */
PaintLayersBakeGateAction BKE_paint_layers_bake_gate_decide(bool stale,
                                                            bool is_invoke,
                                                            bool headless);

/**
 * Hash of the subtree and parameters \a layer's bake depends on, the validity key: two 32-bit
 * words, low first. Pixel edits are not visible here; they arrive through the partial-update
 * subscription and settle as a re-bake of the changed rectangle.
 */
void BKE_paint_layers_bake_hash(const Material &ma,
                                const MaterialPaintLayer &layer,
                                uint32_t r_hash[2]);

/**
 * Whether \a layer's stored bake can stand in for its live subtree right now: a bake exists, its
 * stored hash matches #BKE_paint_layers_bake_hash and the runtime subscription has reported no
 * change to the source maps since the bake. The subscription is runtime-only: a freshly loaded file
 * has none, which is why a load clears every stored hash and the first plan re-bakes.
 */
bool BKE_paint_layers_bake_is_valid(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Stamp \a layer's bake hash from its description and start the pixel-edit subscription, after its
 * maps were filled. This is what makes #paint_layer_row_substituted take the row.
 */
void BKE_paint_layers_bake_finalize(Material &ma, MaterialPaintLayer &layer);

/**
 * Whether \a layer must stay live for bake reasons: a UV mapping applies to it or anything inside
 * it (a bake would freeze the slider values), or a row inside it has an object mask (the choice is
 * made in the shader, after every shared cache). The row's own object mask does not block.
 */
bool BKE_paint_layers_bake_blocked(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether \a layer must stay live right now rather than be baked: it is the active row, or an
 * ancestor of it, so the user is editing inside it and a bake would fight the edit.
 */
bool BKE_paint_layers_bake_row_is_deferred(const Material &ma, const MaterialPaintLayer &layer);

/** The map side at or above which a bake counts as heavy regardless of the subtree's weight. */
constexpr int PAINT_LAYERS_HEAVY_BAKE_SIZE = 2048;

/**
 * Whether \a layer's bake is heavy enough to leave the main thread: its subtree would add more
 * than a fixed number of generated nodes, or its declared map size is at least
 * #PAINT_LAYERS_HEAVY_BAKE_SIZE. A light row is baked synchronously by the planner; a heavy one by
 * the worker job.
 */
bool BKE_paint_layers_bake_is_heavy(const Material &ma, const MaterialPaintLayer &layer);

/**
 * The level of \a layer among the folders around it, a folder in the stack root being 1. The AUTO
 * mode bakes only folders nested at level 2 or deeper; a folder in the root is what the user keeps
 * working in.
 */
int BKE_paint_layers_folder_level(const Material &ma, const MaterialPaintLayer &layer);
bool BKE_paint_layers_folder_auto_bake_allowed(const Material &ma, const MaterialPaintLayer &layer);

/** Whether any heavy, not-yet-valid baked row of \a ma waits for a worker. */
bool BKE_paint_layers_bake_heavy_pending(const Material &ma);

/** Release the runtime \a ma's bake keeps outside the file (subscriptions, remembered sizes). */
void BKE_paint_layers_bake_runtime_free(Material &ma);

/**
 * Hand the bake runtime of \a src to \a dst when the undo system replaces a material with a
 * re-read copy that keeps the same `session_uid`; without it the old ID's free path would drop the
 * entries the new ID still uses.
 */
void BKE_paint_layers_bake_runtime_owner_transfer(Material &dst, Material &src);

/**
 * A heavy-bake job: the description is localized on the main thread at creation, the pixels are
 * computed from the copy on the worker (#BKE_paint_layers_bake_job_compute), and the maps are
 * written back to the live material on the main thread (#BKE_paint_layers_bake_job_commit).
 */
struct PaintLayersBakeJob;

/** A job for every due heavy row of \a ma, or null when nothing can be queued. Main thread. */
PaintLayersBakeJob *BKE_paint_layers_bake_job_create(Main &bmain, Material &ma);

/**
 * The worker half: reads only the localized copy and shared images. \a report_progress gets 0..1;
 * \a is_cancelled is polled between channels, and a cancelled job writes nothing (see
 * #BKE_paint_layers_bake_job_commit).
 */
void BKE_paint_layers_bake_job_compute(PaintLayersBakeJob &job,
                                       FunctionRef<void(float progress)> report_progress,
                                       FunctionRef<bool()> is_cancelled);

/**
 * The main-thread half: write the rendered maps into the live material and stamp their hashes.
 * \return true when some row was written, so the caller notifies the shading.
 */
bool BKE_paint_layers_bake_job_commit(PaintLayersBakeJob &job);

/** Release the job and its localized copy. */
void BKE_paint_layers_bake_job_free(PaintLayersBakeJob &job);

/** \} */

}  // namespace blender
