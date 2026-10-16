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
 * written before the field existed reads the same set it always offered. Base Color is always in
 * the returned set, whatever is stored. This is a read-only view of the description; it never
 * creates a channel record.
 */
uint16_t BKE_paint_layers_channel_set_mask_get(const Material &ma);

/** Whether \a channel is in \a ma's global paint channel set. */
bool BKE_paint_layers_channel_in_set(const Material &ma, eMaterialPaintChannel channel);

/**
 * Add or remove \a channel from \a ma's global paint channel set, materializing the derived set on
 * the first explicit write (a zero field becomes the computed mask, then the edit is applied).
 *
 * The layers are never touched: no channel record or map is created or removed, so switching a
 * channel off keeps every layer's data and switching one on does not author records. Base Color
 * cannot be switched off -- it is the material's constant -- and a request to do so is refused.
 *
 * \return false when \a channel is out of range or Base Color is to be disabled.
 */
bool BKE_paint_layers_channel_set_enable(Material &ma,
                                         eMaterialPaintChannel channel,
                                         bool enabled);

/**
 * Write \a ma's global paint channel set as a whole bitmask, forcing Base Color in and tagging the
 * material once. The layers are never touched; see #BKE_paint_layers_channel_set_enable.
 */
void BKE_paint_layers_channel_set_mask_set(Material &ma, uint16_t mask);

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

}  // namespace blender
