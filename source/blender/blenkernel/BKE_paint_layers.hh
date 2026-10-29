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

#include "BKE_paint_layers_target.hh"

#include "DNA_listBase.h"
#include "DNA_uuid_types.h"

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerChannel;
struct MaterialPaintLayerCurvePatch;
struct MaterialPaintLayerIdColor;
struct MaterialSourceResolve;
struct Brush;
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
 * Visit every row of \a ma that reads another material: the Material rows of the stack and the
 * Material corrections laid over any row, a folder's included. Each is visited with its place in
 * the description only; use #BKE_paint_layers_correction_owner for a correction's row. Return
 * false from \a fn to stop.
 */
void BKE_paint_layers_foreach_source_row(const Material &ma,
                                         FunctionRef<bool(const MaterialPaintLayer &row)> fn);
void BKE_paint_layers_foreach_source_row(Material &ma,
                                         FunctionRef<bool(MaterialPaintLayer &row)> fn);

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
 * Metallic, Roughness, Normal, AO). What a zero #Material::paint_layers_channels field derives. The
 * New Layered Material dialog proposes #BKE_paint_layers_new_material_channel_set instead, which
 * adds Height.
 */
uint16_t BKE_paint_layers_default_channel_set();

/**
 * The channel set a brand-new layered material starts from: the build default plus Height. Only the
 * authoring path (#MATERIAL_OT_new_layered) uses it, so a stack that opens from a file without an
 * authored set keeps #BKE_paint_layers_channel_set_mask_get's build default and never gains Height
 * on its own. The extra channel is stored on creation, so a set read back is an authored one.
 */
uint16_t BKE_paint_layers_new_material_channel_set();

/**
 * Give a brand-new layered material the response values a freshly created stack starts from: Bump
 * Strength five and Bump Distance one (the fields store the distance from one, so Strength is four
 * and the other two are zero). Only the authoring path (#MATERIAL_OT_new_layered) calls it; a file
 * written before the fields existed keeps its zeroes, which read as one, and Normal Strength stays
 * one.
 */
void BKE_paint_layers_new_material_response_apply(Material &ma);

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
 * How many channels \a layer takes part in, the measure of the nodes it adds to the generated tree.
 *
 * Follows the same participation rule the generator uses (#paint_layer_row_participates), so a
 * record that carries neither a map nor any alpha counts for nothing: it wires no channel and costs
 * no node. The UI reads this to warn about a row heavy enough to bake in AUTO mode; a fresh row
 * whose records are all empty weighs zero, however wide the material's channel set is.
 */
int BKE_paint_layers_layer_channel_weight(const Material &ma, const MaterialPaintLayer &layer);

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
 * Give \a layer a mask that reads \a image: the white base mask an Add White Mask makes, and a
 * Fill correction over it whose Base Color record carries the image -- its grey and its alpha -- as
 * the coverage it lays, the same pair the Outliner's drop of an image makes. A Mask-mode stroke
 * paints the map the correction holds.
 *
 * The caller keeps its own user for \a image; the correction's record takes one of its own.
 *
 * \return the new correction, or null when \a layer is not part of \a ma or already has a mask.
 */
MaterialPaintLayer *BKE_paint_layers_mask_add_image(Material &ma,
                                                   MaterialPaintLayer &layer,
                                                   Image &image);

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
 * \a value; a Gradient correction (the Generator source) lays its ramp's grey, its alpha and its
 * line. Like a row's, its opacity, blend and visibility are its own settings.
 *
 * \param name: the correction's name, or null for "Mask Paint" / "Mask Fill" / "Mask Gradient".
 * \return the new item, or null when \a layer is not a row of \a ma's stack, has no base mask, or
 * \a source is not Image, Constant or Generator.
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
 * A Gradient correction (the Generator source) has no map and one channel record: its ramp
 * supplies the colour in a colour channel and its line the mix in a scalar one.
 *
 * \param source: #MA_PAINT_LAYER_SOURCE_IMAGE, #MA_PAINT_LAYER_SOURCE_CONSTANT or
 *                #MA_PAINT_LAYER_SOURCE_GENERATOR.
 * \return the new correction, or null when \a owner is not a row of \a ma's stack or \a source is
 * none of the three. A folder takes one: it corrects the isolated result of what the folder holds,
 * before the folder's own blend, and a folder that has one is no longer a Pass Through folder.
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
 * Add a content correction to \a owner that lays what the material \a source shows: in each
 * channel the source supplies, its plain value, its plain texture, or its graph, over the row's
 * colour, at the correction's opacity and blend. It owns no channel records, so a stroke has
 * nothing to write into; the Material row's rules decide, channel by channel, what is laid
 * (see #BKE_paint_layers_material_row_due and the Material source of a row).
 *
 * A source whose transparency is a map lays nothing yet: the correction has no coverage of its own
 * to carry one.
 *
 * \return the new correction, or null when \a owner is not a row of \a ma's stack, \a source is
 * null, or it is \a ma or already reads it (a cycle).
 */
MaterialPaintLayer *BKE_paint_layers_correction_add_material(Material &ma,
                                                             MaterialPaintLayer *owner,
                                                             const char *name,
                                                             Material *source);

/**
 * Create the owned payload of a Curve Patch row or effect: an empty PaintVector and a private
 * brush snapshot. The snapshot is a full duplicate of \a source_brush when that brush's type can
 * stamp a patch, and a fresh sculpt brush otherwise (null included), named
 * `".CP Brush <row_name>"`, set to the Curve Patch stroke method and given the material-paint
 * record the image effect reads its channel values from.
 *
 * \return the payload, or null when the database cannot create the brush (the container is freed
 * again). The caller links it into its row and seeds the first curve.
 */
MaterialPaintLayerCurvePatch *BKE_paint_layers_curve_patch_payload_create(
    Main &bmain, const char *row_name, const Brush *source_brush);

/**
 * Replace \a row's private brush snapshot with a fresh duplicate of \a source. The old snapshot
 * loses the row's user and is freed when that was its last one; the row and its curves stay as
 * they are (the frozen per-curve parameters belong to the curves, not to the brush). The bake is
 * invalidated because the brush is part of its input.
 *
 * \return false when \a row is not a Curve Patch row or the duplicate could not be made.
 */
bool BKE_paint_layers_curve_patch_brush_set(Material &ma,
                                            Main &bmain,
                                            MaterialPaintLayer &row,
                                            const Brush &source);

/**
 * Add a Curve Patch content effect to \a owner: its channels are laid by the 3D control curves of
 * the effect's own PaintVector, stamped through a private brush snapshot (a full duplicate of \a
 * snapshot_source when its type can stamp a patch, a fresh sculpt brush otherwise), over the row's
 * colour, below the row's own blend. The payload is created with the effect; the seeded control
 * curve is the caller's business, through the editors wrapper
 * (#ED_paint_layers_curve_patch_row_seed_curve).
 *
 * \param bmain: the database the payload's two IDs are created in.
 * \param snapshot_source: the brush duplicated as the effect's tool state, or null for a fresh
 * default one.
 * \return the new effect, or null when \a owner is not a row of \a ma's stack or \a bmain is
 * null. A folder takes one, like any correction.
 */
MaterialPaintLayer *BKE_paint_layers_curve_patch_effect_add(Material &ma,
                                                            MaterialPaintLayer *owner,
                                                            const char *name,
                                                            Main &bmain,
                                                            const Brush *snapshot_source);

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

/** The list an Anchor Point or reader is added to: a row of the stack, a row's effects or its mask. */
enum class PaintLayerAnchorPointList : int8_t {
  Row = 0,
  Effects = 1,
  MaskStack = 2,
};

/**
 * Add an Anchor Point (\a reader false) or an Anchor Point Reader (\a reader true) to \a owner's
 * effects or mask stack, or as a row of the stack. \a insert_after names the item the new one
 * follows in its list, or null for the end; an item that is not in that list is ignored, so the
 * point is appended. A reader keeps \a source as the point it reads.
 *
 * \return the new row, or null when \a owner is not part of \a ma or the target list cannot hold it.
 */
MaterialPaintLayer *BKE_paint_layers_anchor_point_add(Material &ma,
                                                      MaterialPaintLayer *owner,
                                                      PaintLayerAnchorPointList list,
                                                      MaterialPaintLayer *insert_after,
                                                      bool reader,
                                                      const char *name,
                                                      const bUUID &source);

/* -------------------------------------------------------------------- */
/** \name Anchor Point
 *
 * A marker row inside a row's effects or mask stack publishes a slice of that row: its content
 * after the corrections above the marker, or its mask after the mask items above it. An Anchor
 * Point Reader lays that slice. The rules live in `paint_layers_anchor_point.cc`; the generator,
 * the CPU composite, the bake and the GPU all resolve a reference through the same functions here,
 * so they cannot disagree about whether a reader takes part or what it reads.
 * \{ */

/** How many Anchor Points and how many readers a material may use in the live path. */
constexpr int PAINT_LAYER_ANCHOR_POINT_LIMIT = 12;
/** How deep a chain of readers over readers may nest in the live path. */
constexpr int PAINT_LAYER_ANCHOR_POINT_DEPTH_MAX = 3;
/** A chain deeper than this is refused outright, not merely moved to the baked path. */
constexpr int PAINT_LAYER_ANCHOR_POINT_DEPTH_HARD = 6;

/** What a marker publishes, decided by the list it sits in. */
enum class PaintLayerAnchorPointTarget : int8_t {
  None = 0,
  /** The row's content: colour, coverage and content alpha after the corrections above it. */
  Content = 1,
  /** The row's mask: one scalar after the mask items above it, the base mask included. */
  Mask = 2,
};

/** Why a reader does not lay the slice it names. */
enum class PaintLayerAnchorPointIssue : int8_t {
  None = 0,
  /** No row of the material carries the marker, or no source is set. */
  NotFound = 1,
  /** The point is built after the reader. */
  Forward = 2,
  /** A correction of a row reads a mask point of that same row; the row's effects are built first. */
  OrderInRow = 3,
  /** The readers form a loop. */
  Cycle = 4,
  /** The slice contains a row with an active object mask; it cannot be baked. */
  ObjectMaskInSlice = 5,
  /** A mask slice in a UDIM material. */
  UdimUnsupported = 6,
  /** The chain of readers is deeper than #PAINT_LAYER_ANCHOR_POINT_DEPTH_MAX. */
  ChainTooDeep = 7,
  /** The material has more points or readers than the live path is limited to. */
  LimitExceeded = 8,
};

/** A short human-readable reason for \a issue; static storage. */
const char *paint_layer_anchor_point_issue_text(PaintLayerAnchorPointIssue issue);

/** The result of resolving a reader's reference. */
struct PaintLayerAnchorPointRef {
  /** The marker that publishes the slice, or null when the reader names none or it is gone. */
  const MaterialPaintLayer *point = nullptr;
  /** The row the marker sits in, or null. */
  const MaterialPaintLayer *source_row = nullptr;
  /** Content or mask, from the list the marker sits in. */
  PaintLayerAnchorPointTarget target = PaintLayerAnchorPointTarget::None;
  /** Why there is no slice, or #PaintLayerAnchorPointIssue::None. */
  PaintLayerAnchorPointIssue issue = PaintLayerAnchorPointIssue::NotFound;
  /** The point's position in the generator's build order, or -1. */
  int rank = -1;
};

/**
 * Resolve the reference of \a reader: the marker it names, the row and list the marker sits in,
 * and whether the reference is allowed. The one place the order rules are written.
 */
PaintLayerAnchorPointRef paint_layer_anchor_point_resolve(const Material &ma,
                                                          const MaterialPaintLayer &reader);

/**
 * The position of \a marker in the order the generator builds the stack: bottom-up, a folder's
 * children before the folder, a Pass Through folder expanded in place, and within a row its
 * effects before its mask stack. -1 when no row carries the marker.
 */
int paint_layer_anchor_point_rank(const Material &ma, const bUUID &marker);

/**
 * Whether \a reader takes part in \a channel through the slice it names: the reference resolves
 * and the slice supplies the channel. A mask slice read as content is a grey in any colour channel
 * but Normal, which has no grey.
 */
bool paint_layer_row_reads_anchor_point(const Material &ma,
                                        const MaterialPaintLayer &reader,
                                        int channel);

/** The readers of the material that name the point \a point, in any role and at any depth. */
blender::Vector<const MaterialPaintLayer *> paint_layer_anchor_point_readers(
    const Material &ma, const bUUID &point);

/** Whether the row of \a marker is an Anchor Point with at least one reader. */
bool paint_layer_anchor_point_is_read(const Material &ma, const bUUID &marker);

/**
 * Whether the row \a layer is a point that keeps its row live: the bake must not substitute the
 * row (or any folder above it) while a point inside it is read, or the point would disappear.
 */
bool paint_layer_anchor_point_blocks_bake(const Material &ma, const MaterialPaintLayer &layer);

/** The number of points and the number of readers \a ma currently has. */
void paint_layer_anchor_point_counts(const Material &ma, int &r_points, int &r_readers);

/**
 * The channel a mask reader takes its coverage from: a content slice's first participating
 * channel, or Base Color for a mask slice.
 */
int paint_layer_anchor_point_mask_read_channel(const Material &ma,
                                               const MaterialPaintLayer &reader);

/** Whether the slice of \a reader would contain a row with an active object mask. */
bool paint_layer_anchor_point_slice_has_object_mask(const Material &ma,
                                                    const MaterialPaintLayer &reader);

/** Whether \a reader reads a mask slice whose source maps are tiled (UDIM). */
bool paint_layer_anchor_point_slice_is_udim(const Material &ma,
                                            const MaterialPaintLayer &reader);

/** How many readers deep the chain of \a reader is: 1 for a reader without nested readers. */
int paint_layer_anchor_point_chain_depth(const Material &ma, const MaterialPaintLayer &reader);

/**
 * Whether \a reader may be pointed at \a marker: the marker is a point of this material that is
 * built before the reader, readable from the reader's list, and creates no loop or chain deeper
 * than #PAINT_LAYER_ANCHOR_POINT_DEPTH_MAX. The choices the UI offers.
 */
bool paint_layer_anchor_point_source_valid(const Material &ma,
                                           const MaterialPaintLayer &reader,
                                           const bUUID &marker);

/** Whether \a row is the inert Anchor Point marker (its kind, not a marker field). */
bool BKE_paint_layers_is_anchor_point(const MaterialPaintLayer &row);

/** Whether \a row reads the slice an Anchor Point publishes. */
bool BKE_paint_layers_reads_anchor_point(const MaterialPaintLayer &row);

/** Set the marker \a reader reads; nil clears it. Refused for a row that is not a reader. */
bool BKE_paint_layers_anchor_point_source_set(Material &ma,
                                              MaterialPaintLayer *reader,
                                              const bUUID &marker);

/** The marker \a reader reads, nil for every row that is not a reader. */
bUUID BKE_paint_layers_anchor_point_source_get(const MaterialPaintLayer &reader);

/**
 * Whether \a layer may carry an object mask: only a row of the stack (never a content correction
 * or a mask item), since the slice's object-mask bounds are read from rows alone.
 */
bool BKE_paint_layers_object_mask_role_allowed(const MaterialPaintLayer &layer);

/**
 * Set \a layer's object-mask type. Rejected (and any sticky flag cleared) for EFFECT and
 * MASK_ITEM roles; only a stack row may carry an object mask.
 * \return false when the role forbids it or the value is unchanged.
 */
bool BKE_paint_layers_object_mask_type_set(Material &ma,
                                           MaterialPaintLayer &layer,
                                           int8_t type);

/** \} */

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

/** The range #BKE_paint_layers_channel_height_scale_set accepts and the RNA slider offers. */
constexpr float PAINT_HEIGHT_SCALE_MIN = 0.0f;
constexpr float PAINT_HEIGHT_SCALE_MAX = 4.0f;

/**
 * The highest response value a material stores, for Bump Strength and Normal Strength; the setters
 * clamp to it and the RNA range offers the same bound.
 */
constexpr float PAINT_LAYERS_RESPONSE_STRENGTH_MAX = 100.0f;
/** The highest Bump Distance, in Blender metres, a material stores. */
constexpr float PAINT_LAYERS_RESPONSE_DISTANCE_MAX = 1000.0f;

/**
 * The Height record's own scale of \a layer's \a channel: `1 + height_scale_delta`, so the default
 * (and the value files written before the field read) is one. Only the Height channel honours it.
 */
float BKE_paint_layers_channel_height_scale(const MaterialPaintLayer &layer, int channel);

/**
 * Set the Height record's own scale of \a layer's \a channel; only the Height channel accepts one,
 * every other channel refuses it. The scale scales the record's distance from the neutral level,
 * so one is neutral. The scale is clamped to #PAINT_HEIGHT_SCALE_MIN..#PAINT_HEIGHT_SCALE_MAX:
 * a negative scale is not offered, because the inversion is the Subtract mode and the Invert
 * action rather than a negative distance.
 *
 * Value-only: the scale is an input of the generated chain, so the edit never rebuilds the
 * topology.
 *
 * \return false when \a layer is not part of \a ma, \a channel is out of range or is not Height.
 */
bool BKE_paint_layers_channel_height_scale_set(Material &ma,
                                               MaterialPaintLayer &layer,
                                               int channel,
                                               float scale);

/**
 * The Height record's own blend of \a layer's \a channel, a #eMaterialPaintLayerHeightBlend; zero
 * inherits the channel's ordinary blend. Only the Height channel honours it.
 */
int BKE_paint_layers_channel_height_blend(const MaterialPaintLayer &layer, int channel);

/**
 * The Height mode \a layer's \a channel really lays: its own mode when one is set, otherwise what
 * the row's ordinary blend maps to. Inherit reads Mix as Inherit (the ordinary Mix is the same step
 * at scale one, and it keeps the GPU preview), Add and Subtract keep their meaning, and every other
 * blend reads as Mix. The one place the rule lives; every reader of the effective mode calls this.
 */
int BKE_paint_layers_channel_height_blend_effective(const MaterialPaintLayer &layer, int channel);

/**
 * Set the Height record's own blend of \a layer's \a channel, a
 * #eMaterialPaintLayerHeightBlend value (0 to inherit the channel's ordinary blend); only the
 * Height channel accepts one, every other channel refuses it.
 *
 * A structural edit: the height modes are different nodes of the generated chain, so the tree is
 * marked stale.
 *
 * \return false when \a layer is not part of \a ma, \a channel is out of range or is not Height.
 */
bool BKE_paint_layers_channel_height_blend_set(Material &ma,
                                               MaterialPaintLayer &layer,
                                               int channel,
                                               int height_blend);

/**
 * Set whether \a layer lays anything in the Height channel. Skipping keeps the row's Height record
 * (its map or flat value stay, the panel keeps showing them) while the height it carries stops
 * contributing to the stack -- the Ucupaint `write_height = False` -- and leaves the row's other
 * channels alone.
 *
 * A structural edit: the row leaves the Height chain of the generated graph, the way a row that
 * stops participating does.
 *
 * \return false when \a layer is not part of \a ma.
 */
bool BKE_paint_layers_channel_height_write_set(Material &ma,
                                               MaterialPaintLayer &layer,
                                               bool write);

/** Whether \a layer lays anything in the Height channel (its records can outlive the choice). */
bool BKE_paint_layers_channel_height_write(const MaterialPaintLayer &layer);

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
 * correction) or through a mask correction's own image, or a Material row whose source reads the
 * object's active UVs in its root tree or in a group on the path to its Principled BSDF
 * (#BKE_paint_layers_source_reads_uv). A Material row moves the UV at which its source is read --
 * its live textures, its rendered channel maps and its source-group wrapper. A map written by
 * strokes (Paint) is addressed at the raw UV and never supports it; neither does a filter, which
 * owns no map, nor the base mask, whose map the Mask brush paints.
 *
 * The mapping predicates, and who asks each (each name is a suffix of
 * #BKE_paint_layers_mapping_*):
 *
 * | Predicate | Question | Callers |
 * | --- | --- | --- |
 * | `supported` | Carries a mapping with its content? | The other three |
 * | `available` | May the UI edit one, even before a map? | Panel, gizmo, enable |
 * | `applies` | Is it switched on and supported? | Values, UI |
 * | `remaps` | Does generation need a transform? | Graph, CPU, hashes |
 */
bool BKE_paint_layers_mapping_supported(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether \a layer's mapping is applied right now: enabled and supported. The values never move
 * this answer; a slider drag must not rebuild anything.
 */
bool BKE_paint_layers_mapping_applies(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether \a layer keeps a UV mapping at all: everything #BKE_paint_layers_mapping_supported
 * accepts, plus a Fill row that has no map yet. A Fill's mapping belongs to the row, not to the
 * map it happens to read: the row is made with its mapping enabled, so the Mapping panel and the
 * Image Editor's mapping gizmo are there before the first texture is slotted, and the values take
 * effect the moment one is. The graph and the CPU ask #BKE_paint_layers_mapping_remaps, so a row
 * with no map builds nothing.
 */
bool BKE_paint_layers_mapping_available(const Material &ma, const MaterialPaintLayer &layer);

/** Whether \a layer's offset, scale and rotation are the identity transform. */
bool BKE_paint_layers_mapping_is_identity(const MaterialPaintLayer &layer);

/**
 * Whether generation needs to remap \a layer: #BKE_paint_layers_mapping_applies, and the transform
 * either moves the UVs or clips them (#BKE_paint_layers_mapping_wrap_get is not Repeat). An
 * identity mapping with Repeat is skipped, so a row whose mapping was switched on before it had a
 * value builds the same graph as an unmapped one.
 */
bool BKE_paint_layers_mapping_remaps(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether strokes may write into \a row's own channels. A Fill row lays a flat value (with an
 * optional map read through its mapping); painting it would turn that map into a stroke target.
 * Its content is edited by corrections instead. A row's mask is still paintable.
 */
bool BKE_paint_layers_row_is_paintable(const MaterialPaintLayer &row);

/** Whether \a layer's mapping is enabled. Every new row starts enabled. */
bool BKE_paint_layers_mapping_enabled_get(const MaterialPaintLayer &layer);

/**
 * Set whether \a layer's mapping is enabled. Refused when #BKE_paint_layers_mapping_available
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
 * Where the middle of \a layer's tile lands on the canvas: the inverse of the sample transform
 * read at the middle of the map. It is the tile's position as the user sees it -- the gizmo's
 * anchor ring -- and the one point the tile-holding edits keep still. Read-only: the offset is
 * the one stored value that places the tile. RNA exposes this as ``mapping_tile_center``, which
 * the Mapping panel's fields read to follow the offset themselves.
 */
void BKE_paint_layers_mapping_tile_center(const MaterialPaintLayer &layer, float r_center[2]);

/**
 * Set the mapping scale of \a layer the way the canvas shows it: the tile of the map stays
 * where it is, its offset following the new scale.
 *
 * The stored offset is added after the scale and the rotation, so changing the scale alone moves
 * the tile across the canvas -- the gizmo's rectangle jumps while the offset reads the same.
 * These calls hold the tile on the canvas point its centre already occupies (the gizmo's anchor
 * ring): the reset operator's Scale and Rotation and the gizmo's lock button answer to them.
 * #BKE_paint_layers_mapping_set_scale is the raw value, for callers that place the tile
 * themselves: the gizmo computes its own offset for a drag, and the Mapping panel's fields keep
 * the centre through #BKE_paint_layers_mapping_tile_center. Value-only.
 */
bool BKE_paint_layers_mapping_set_scale_holding_tile(Material &ma,
                                                     MaterialPaintLayer *layer,
                                                     const float scale[2]);

/** As #BKE_paint_layers_mapping_set_scale_holding_tile, for the rotation: the tile turns around
 * its centre without leaving it. Value-only. */
bool BKE_paint_layers_mapping_set_rotation_holding_tile(Material &ma,
                                                        MaterialPaintLayer *layer,
                                                        float rotation);

/** As #BKE_paint_layers_mapping_set_scale_holding_tile, for the scale lock: locking joins the axes
 * at the X value, which is a scale change. Value-only. */
bool BKE_paint_layers_mapping_set_scale_lock_holding_tile(Material &ma,
                                                          MaterialPaintLayer *layer,
                                                          bool locked);

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
 * Whether a mask correction that reads a material can take its grey from \a channel: a colour or a
 * scalar of the source. Normal is a vector, Alpha is the item's weight, and Custom is a vertex
 * attribute, so none of the three is a grey.
 */
bool BKE_paint_layers_mask_source_channel_allowed(int channel);

/**
 * The channel of the source the mask correction \a item takes its grey from (Base Color by
 * default), or -1 when \a item is not a mask correction that reads a material.
 */
int BKE_paint_layers_mask_source_channel_get(const MaterialPaintLayer &item);

/**
 * Take the grey of the mask correction \a item from \a channel of its source. The maps rendered
 * from the source are let go of: they were made for the old channel, and the row renders again.
 * Refused for an item that is not a mask correction reading a material of \a ma, and for a channel
 * #BKE_paint_layers_mask_source_channel_allowed refuses.
 */
bool BKE_paint_layers_mask_source_channel_set(Material &ma, MaterialPaintLayer *item, int channel);

/**
 * Whether the material \a layer reads is a directly linked source that
 * #BKE_paint_layers_material_source_make_local can take: \a layer is a Material row, or a content
 * correction that reads a material, of an editable, non-override \a ma. A source that is merely an indirect dependency is not the user's to take.
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
 * Whether the Material row \a row has to be rendered now although the planner would not ask for
 * it: its source supplies a channel as a graph, the row's bake mode is not Never, and its maps are
 * missing or stale. The planner leaves an edited row and a row shown live in Auto to wait; a
 * consumer that reads the rendered maps ("Use Row Result") cannot.
 */
bool BKE_paint_layers_material_row_render_forced_wanted(const Material &ma,
                                                        const MaterialPaintLayer &row);

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
 * Add a mask correction to \a layer that lays the grey of what the material \a source shows: the
 * mean of its Base Color (its plain value or texture, or the map its graph is rendered into), with
 * the colour's own alpha and the source's plain transparency as the weight. It owns no channel
 * records. A source whose transparency is a map lays nothing yet. The row needs a base mask first,
 * as for any mask correction.
 * \return The new item, or null when \a layer has no base mask, is not in the stack, \a source is
 * null, or it is the material itself or already reads it (a cycle).
 */
MaterialPaintLayer *BKE_paint_layers_mask_material_add(Material &ma,
                                                       MaterialPaintLayer *layer,
                                                       const char *name,
                                                       Material *source);

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
  /**
   * A row names an Anchor Point that cannot give it a slice: the point is gone, is built after the
   * reader, sits in a list the reader cannot read from its row, forms a loop, or the slice cannot
   * be baked. The row takes part nowhere; the text says which rule failed.
   */
  AnchorPointInvalid = 6,
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
/** \name Painting lock
 * \{ */

/** Whether \a layer is protected from painting (#MA_PAINT_LAYER_LOCKED). */
bool BKE_paint_layers_is_locked(const MaterialPaintLayer &layer);

/**
 * Set or clear the painting lock of \a layer.
 *
 * The lock changes no pixel, no value the stack is evaluated with and no topology, so it only tags
 * the material for a redraw: the paint target answers it on the next resolve. A row of another
 * material is refused.
 */
bool BKE_paint_layers_set_locked(Material &ma, MaterialPaintLayer *layer, bool locked);

/**
 * The row that keeps \a row from being painted because it is locked: \a row itself, the stack row a
 * mask item or correction hangs on, or one of that row's parent folders. Null when nothing locks it.
 * \a r_reason, when given, names where the lock sits, so a report never re-walks the chain.
 */
const MaterialPaintLayer *BKE_paint_layers_lock_blocker(
    const Material &ma,
    const MaterialPaintLayer &row,
    PaintLayersBlockerPlace *r_reason = nullptr);

/**
 * The row that silences \a row because its visibility is switched off (#MA_PAINT_LAYER_ENABLED):
 * \a row itself, the stack row a mask item or correction hangs on, or one of that row's parent
 * folders. Null when the whole chain shows. \a r_reason, when given, names where the switch sits,
 * so a report never re-walks the chain.
 *
 * A stroke into a hidden row paints a map the render never sees, so the paint tools refuse it the
 * way they refuse a locked one (see #BKE_paint_layers_lock_blocker).
 *
 * A mask item's own switch is the exception: for the item itself this still answers Self, but
 * #BKE_paint_layers_target_resolve does not refuse it; the item stays the stroke target and only
 * refuses softly through #BKE_paint_layers_target_is_live. A caller deciding whether a stroke may
 * start reads the target status, not this answer alone.
 *
 * A correction (role #MA_PAINT_LAYER_ROLE_EFFECT) carries its own switch like a row. A row that
 * takes no stroke anyway (a filter) keeps its RowTakesNoStrokes answer from the target resolve.
 */
const MaterialPaintLayer *BKE_paint_layers_hidden_blocker(
    const Material &ma,
    const MaterialPaintLayer &row,
    PaintLayersBlockerPlace *r_reason = nullptr);

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
 * Every map \a layer's bake reads: its own channel maps and those of its corrections, mask items
 * and children. A Material row reads the images of its source's node trees when its own bake is
 * the question (\a is_child false), and the maps it shows when it is a child of the row being
 * baked.
 *
 * Shared with consumers that cache a render of the row outside the planner, so a pixel edit marks
 * their cache stale exactly as it would the row's own bake.
 */
void BKE_paint_layers_bake_collect_source_images(const Material &ma,
                                                 const MaterialPaintLayer &layer,
                                                 Vector<Image *> &r_images,
                                                 bool is_child = false);

/**
 * Composite \a layer's subtree for \a channel the way a bake of it would: the isolated-group model
 * -- the straight colour, the content alpha in its fourth component and the coverage it lays by --
 * into \a size x \a size buffers. The planner and the generator's substitution read this one
 * function's result, so a baked map means one thing. Source maps are composited at their own
 * resolution and nearest-sampled to \a size.
 *
 * \param dst_rect: (x0, y0, x1, y1) in result pixels; when given only that rectangle is computed
 * and written, which is how a partial re-bake touches one changed tile.
 * \return false when the row has nothing to render in \a channel.
 */
bool BKE_paint_layers_bake_render_node(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       int channel,
                                       int size,
                                       float *r_color_rgba,
                                       float *r_coverage_gray,
                                       const int *dst_rect = nullptr);

/** The pixel dimensions of \a layer's content in \a channel, the size a bake defaults to. */
bool BKE_paint_layers_row_dimensions(const Material &ma,
                                     const MaterialPaintLayer &layer,
                                     int channel,
                                     int &r_width,
                                     int &r_height);

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

/**
 * A "Use Row Result" job: render one row (\a source_marker) of the layered \a ma for every channel
 * it takes part in, and add the results as channel maps of another row (\a target_marker).
 *
 * Same split as #PaintLayersBakeJob -- the description is localized at creation, the pixels are
 * computed on the worker, and the maps are created and written on the main thread -- because a
 * heavy row at 2048+ pixels is too slow to render inside the operator.
 */
struct PaintLayersRowResultJob;

/** Localize \a ma and remember the two rows; null when either is missing or \a ma is not layered.
 */
PaintLayersRowResultJob *BKE_paint_layers_row_result_job_create(
    Main &bmain, Material &ma, const bUUID &source_marker, const bUUID &target_marker, int size);

/** Render \a source_marker's channels from the localized copy. Runs on the worker thread. */
void BKE_paint_layers_row_result_job_compute(PaintLayersRowResultJob &job);

/**
 * Create the target row's maps from the computed buffers and link them. Runs on the main thread.
 *
 * \return whether any channel was committed; false too when the material or the target row is gone
 * or the target takes no channel records (a folder or a Material row).
 */
bool BKE_paint_layers_row_result_job_commit(PaintLayersRowResultJob &job);

/** Release the job and its localized copy. */
void BKE_paint_layers_row_result_job_free(PaintLayersRowResultJob &job);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal maps
 *
 * The maps the stack mints for a row (its painted channels, a bake, a merge, a mask whose kind
 * paints its own map) are the stack's own: the user reaches them through the layer UI alone, so
 * the image browsers hide them unless Developer Extras is on. A map the user assigned -- a dropped
 * texture, an ID Map a filter samples -- is never internal.
 * \{ */

/**
 * Whether \a image carries #IMA_PAINT_LAYER_INTERNAL_MAP: the stack generated and owns it. The flag
 * is written where the map is created, and #BKE_paint_layers_internal_maps_mark adds it to files
 * saved before it existed.
 */
bool BKE_paint_layers_image_is_internal(const Image &image);

/**
 * Mark \a image as a map the stack generated and owns. This is the only place the flag is set:
 * every creation path calls it, so a map the stack mints cannot be forgotten in the browsers.
 */
void BKE_paint_layers_image_mark_internal(Image &image);

/**
 * Mark every map the stack owns in \a bmain as internal. Maps of older files were made before the
 * flag existed, so loading one needs the walk. A user-assigned map is left alone: the predicate is
 * the row's kind, not merely the presence of an image in a channel record.
 *
 * Idempotent; safe to call on any file, including one that already has every map marked.
 */
void BKE_paint_layers_internal_maps_mark(Main &bmain);

/** \} */

}  // namespace blender
