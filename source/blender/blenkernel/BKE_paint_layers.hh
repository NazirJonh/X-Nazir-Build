/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The description of a layered material's stack: adding, removing, looking rows up and editing
 * them in place.
 *
 * A layered material keeps its stack as DNA (`Material::paint_layers`), not as a node graph; the
 * graph is generated from this description (phase 1). Everything here is therefore
 * description-only and never touches a node tree. Rows are addressed by their
 * #MaterialPaintLayer::marker, never by position: an index is ambiguous across nesting, while a
 * marker survives reordering and copying.
 *
 * Every mutation marks #MA_PAINT_LAYERS_REGEN and tags the material's shading, so the generated
 * tree is rebuilt. Setting the active marker does not: it moves a cursor, not the stack, and the
 * tree is generated from topology alone.
 */

#include <cstdint>
#include <memory>

#include "BKE_paint_material_resolve.hh"

#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_vector.hh"
#include "DNA_uuid_types.h"

namespace blender {

struct Image;
struct ImageUser;
struct ImBuf;
struct Main;
struct Material;
struct Mesh;
struct Object;
struct MaterialPaintLayer;
struct MaterialPaintLayerBake;
struct MaterialPaintLayerChannel;
struct PaintLayersRegenCache;
struct bNodeTree;
enum eMaterialPaintChannel : int8_t;
enum eMaterialPaintLayerSource : int8_t;
enum eMaterialPaintLayerBlend : int8_t;

template<typename T> class Span;

/* -------------------------------------------------------------------- */
/** \name Layered material and its UV map
 * \{ */

/**
 * Whether \a ma is a layered material: its stack is the DNA description (#MA_PAINT_LAYERED) and
 * its node graph is generated from it (phase 1).
 *
 * The old graph-as-truth path asks this before parsing or writing anything, so a layered material
 * is refused rather than read as a stack it is not. This is the one flag test every such guard
 * shares; the description API here is what a layered material is edited through.
 */
bool paint_layers_is_layered(const Material &ma);

/**
 * The one UV layer a Paint Layers stack samples on \a mesh, the single point of choice for the
 * generated graph, painting, baking and the mesh-map hash.
 *
 * When the material names a UV layer and \a mesh has it, the name is returned. When the material
 * names one and \a mesh does not have it, null is returned and \a r_missing (when non-null) is set
 * to true: a caller must not silently substitute another layer. With no name set the mesh's active
 * UV map is returned, preserving the behavior before names existed.
 */
const char *BKE_paint_layers_uv_map_resolve(const Mesh &mesh,
                                            const Material &ma,
                                            bool *r_missing);

/**
 * Fill #Material::paint_layers_uv_map from \a ob's active UV map when it is still empty, so a
 * material that has just become layered samples the UV the object is unwrapped with. \a ob may be
 * null, or not a mesh object, or carry no UV layer; then the name is left empty and the active-UV
 * fallback of #BKE_paint_layers_uv_map_resolve applies.
 *
 * \a ob must use \a ma in one of its material slots (object or data, per
 * #BKE_object_material_index_get): a caller's context object can be unrelated to the material
 * (another object's slot, an override, an addon holding the wrong pointer), and naming the stack
 * after that object's mesh would fix it to a layer its real users do not have. A material already
 * naming a layer is never overwritten either way.
 */
void BKE_paint_layers_uv_map_autofill(Material &ma, const Object *ob);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Rows, roles and sources
 * \{ */

/**
 * Whether \a layer is a folder: a row that holds its stack in #MaterialPaintLayer::children and
 * takes part in a channel through them alone.
 *
 * This is the single test for folder-ness everywhere -- the generator, the CPU compositor, the
 * channel rules and the UI -- so a row can never be a folder on one side and a leaf on another. It
 * reads #MaterialPaintLayer::source (#MA_PAINT_LAYER_SOURCE_STACK), never a non-empty #children
 * list: an emptied folder stays a folder, and a malformed leaf that somehow has children is still
 * a leaf.
 */
bool BKE_paint_layers_is_folder(const MaterialPaintLayer &layer);

/**
 * A row's structural place in its owner, independent of what it reads from.
 *
 * A Layer is a stack member; an Effect adjusts its owner's colour (a former content correction); a
 * MaskItem limits its owner's coverage (a former mask correction). Phase 2.2 stores effects and
 * mask items in lists of their own; this role is what chooses between them.
 */
enum class PaintLayerRole : int8_t {
  Layer = 0,
  Effect,
  MaskItem,
};

/** The #PaintLayerRole \a layer takes: reads #MaterialPaintLayer::role directly. */
PaintLayerRole BKE_paint_layers_role(const MaterialPaintLayer &layer);

/**
 * The effect rows of \a layer, bottom to top: its #MaterialPaintLayer::effects list. An effect
 * adjusts the colour the row paints with, before its blend.
 */
Vector<MaterialPaintLayer *> BKE_paint_layers_effects(MaterialPaintLayer &layer);
Vector<const MaterialPaintLayer *> BKE_paint_layers_effects(const MaterialPaintLayer &layer);

/**
 * The mask items of \a layer, bottom to top: its #MaterialPaintLayer::mask_stack list. A mask item
 * limits where the row applies; the stack's coverage starts at one.
 */
Vector<MaterialPaintLayer *> BKE_paint_layers_mask_items(MaterialPaintLayer &layer);
Vector<const MaterialPaintLayer *> BKE_paint_layers_mask_items(const MaterialPaintLayer &layer);

/**
 * The layer's base mask item (#MA_PAINT_LAYER_MASK_BASE), or null when it has none.
 *
 * The base mask is always the first item of \a layer's mask stack while it exists; a layer with no
 * base mask may still carry mask corrections.
 */
MaterialPaintLayer *BKE_paint_layers_mask_base(MaterialPaintLayer &layer);
const MaterialPaintLayer *BKE_paint_layers_mask_base(const MaterialPaintLayer &layer);

/**
 * A flat walk of \a ma's description, bottom to top, with folders emitted and then their children.
 *
 * This is a traversal for identity and participation only, *not* the compositing parser: folders
 * are not collapsed away and no blend, mask or coverage semantics live here. The CPU composite
 * reads the stack through #BKE_paint_layers_composite_image_layers, which knows about isolated
 * folders; the generator and the issue list are the callers of this walk. Correction rows are
 * reached through their owner's `effects`/`mask_stack` lists, not here; Custom and Material layers
 * are walked like any other row and take part through their bake.
 */
void BKE_paint_layers_flatten(const Material &ma, Vector<const MaterialPaintLayer *> &r_layers);

/**
 * Every row of \a ma's description, of any role, depth-first: a row, then its effects, then its
 * mask stack, then its children, each walked recursively (a mask item may itself carry effects,
 * a mask stack and children, to unbounded depth).
 *
 * Unlike #BKE_paint_layers_flatten, this does not skip corrections and mask items, so it is the
 * walk the depsgraph and bake queue use to reach every row that can hold a source (including
 * inside effects/mask_stack) instead of only the composited stack. The compositor keeps using
 * #BKE_paint_layers_flatten.
 */
void BKE_paint_layers_flatten_all(const Material &ma, Vector<const MaterialPaintLayer *> &r_rows);

/** A description edit: the generated tree is stale until it is rebuilt. */
void BKE_paint_layers_tag_edited(Material &ma);

/**
 * The value \a channel's stack starts from, before any row is laid over it: the shader-space RGBA
 * the generated chain's bottom constant holds and a CPU composite must initialise its buffer to.
 *
 * The two must read this one table, or a partially covered row would fade towards different values
 * on the GPU and on the CPU. The values are the channel's neutral default -- what the Principled
 * shows where nothing covers it -- and the alpha is used only where a channel has one.
 */
void BKE_paint_layers_channel_bottom_color(eMaterialPaintChannel channel, float r_color[4]);

/**
 * The Base Color \a layer shows: a Layer-role row's Base-Color channel record (its single stored
 * constant, whether the row is a Fill or a Paint), or the DNA #MaterialPaintLayer::fill_color of a
 * row without channel records (a mask or Fill-effect correction).
 *
 * The one reader of the Base-Color constant for code that has only the row, not a channel index --
 * the Outliner swatch, the row hash and the RNA `fill_color` property. Anything that reads a
 * channel constant directly must go through `paint_layer_channel_constant` instead.
 */
void BKE_paint_layers_base_color_get(const MaterialPaintLayer &layer, float r_color[4]);

/**
 * Whether the Base Color #BKE_paint_layers_base_color_get reads is a live one: a Layer-role row's
 * Base-Color record exists and is ENABLED, while any other row's DNA field has no state to switch
 * off and is always active.
 *
 * The UI asks this before drawing a colour control that would stand for a channel the row no longer
 * paints with -- a swatch or picker over a removed or switched-off Base Color would claim the row
 * still fills. The stored colour stays readable through #BKE_paint_layers_base_color_get either way.
 */
bool BKE_paint_layers_base_color_active(const MaterialPaintLayer &layer);

/** Why a row of a layered material behaves differently than its settings suggest. */
enum class PaintLayersIssueCode : int8_t {
  /**
   * A content Fill correction under a layer that carries the Normal channel. A constant normal has
   * no meaning, so the generator and the CPU composite both skip the correction in that channel.
   */
  FillCorrectionOnNormal = 0,
  /**
   * A folder carrying a map of its own. A folder takes part in a channel through its children, so
   * a map on the folder itself is ignored; a record without a map is fine, it carries the pair's
   * blend/opacity settings.
   */
  FolderHasMaps = 1,
  /**
   * A row that is not a folder yet holds children. Only a folder takes part through its children,
   * so the stray nesting is ignored; this catches a malformed file or an edit made around the API.
   */
  NonFolderHasChildren = 2,
  /** A Custom layer's group carries an interface socket whose role is not a known one. */
  CustomUnknownRole = 3,
  /** A Custom role is on a socket of the wrong direction (an input role on an output, or back). */
  CustomRoleDirection = 4,
  /** A Custom group declares the same role -- or the same COLOR channel -- twice. */
  CustomDuplicateRole = 5,
  /** A Custom role's socket type does not match the channel it names. */
  CustomSocketType = 6,
  /** A Custom `COLOR:`/`BELOW:` role names a channel the paint system does not have. */
  CustomUnknownChannel = 7,
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
 * so it is cheap enough to call wherever a row is drawn. For a Custom layer it also validates the
 * group's `pbr_custom_role` interface contract.
 */
void BKE_paint_layers_issues_get(const Material &ma, Vector<PaintLayersIssue> &r_issues);

/**
 * The channels a Custom \a layer's group declares through its `COLOR:<CHANNEL>` outputs, in
 * interface order. Empty for a layer that is not Custom or has no group.
 */
void BKE_paint_layers_custom_channels_get(const MaterialPaintLayer &layer, Vector<int> &r_channels);

/**
 * The channels of \a layer's own bake that currently hold a map, in #eMaterialPaintChannel order.
 * The coverage map stands for `PAINT_MATERIAL_CHANNEL_ALPHA`. Empty without a valid bake, or for a
 * Material row (whose bake is its source's maps, resolved through the channels instead).
 */
void BKE_paint_layers_baked_channels_get(const Material &ma,
                                         const MaterialPaintLayer &layer,
                                         Vector<int> &r_channels);

/** The kinds a Custom group's `pbr_custom_role` name can stand for. */
enum class PaintLayerCustomRole : int8_t {
  /** No role at all: a user parameter. */
  None = 0,
  Below,
  Color,
  Coverage,
  CoverageBelow,
  UV,
  /** A non-empty role the contract does not define. */
  Unknown,
};

/** The kind \a role names, or #PaintLayerCustomRole::Unknown for a role not in the contract. */
PaintLayerCustomRole BKE_paint_layers_custom_role_kind(const char *role);

/** The channel a `COLOR:<CHANNEL>`/`BELOW:<CHANNEL>` \a role names, or -1 for none. */
int BKE_paint_layers_custom_role_channel(const char *role);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Description edits
 * \{ */

/**
 * Where #BKE_paint_layers_add puts the new row relative to its anchor.
 *
 * Mirrors the `place` names the RNA/design uses (`'ABOVE'` and friends).
 */
enum class PaintLayerPlace : int8_t {
  /** Directly above the anchor, in the anchor's own list. */
  Above = 0,
  /** Directly below the anchor, in the anchor's own list. */
  Below,
  /**
   * Inside the anchor, which has to be a folder: appended on top of what it holds.
   *
   * A place of its own rather than "above the folder's top row", because an empty folder has no
   * row to name -- and an empty folder is exactly what a folder is when it is made to be filled.
   */
  Into,
};

/**
 * Add a description row to \a ma.
 *
 * With a null \a anchor the row is appended on top of the top-level list; otherwise it is placed
 * relative to \a anchor by \a place. #PaintLayerPlace::Into requires \a anchor to already be a
 * folder (#BKE_paint_layers_is_folder): the row goes on top of whatever it holds. A non-folder is
 * refused rather than promoted -- a source is never changed implicitly, and promoting an Image or
 * Constant layer would make its maps ignored. A UI that drops "into" a plain row groups instead
 * (see #BKE_paint_layers_group). The row is
 * given a fresh #bUUID marker unique within \a ma, and the usual defaults: the \a source, Mix
 * blend, enabled, full opacity.
 *
 * Sets #MA_PAINT_LAYERED on \a ma -- a material that owns a description is a layered material, and
 * the old graph-as-truth path refuses it from then on -- and marks the generated tree stale; see
 * the file comment.
 *
 * \return the new row, or null when \a anchor is non-null but is not part of \a ma's stack. No
 * other call may place a row under a foreign anchor, so all three places validate it first.
 */
MaterialPaintLayer *BKE_paint_layers_add(Material &ma,
                                         eMaterialPaintLayerSource source,
                                         const char *name,
                                         MaterialPaintLayer *anchor,
                                         PaintLayerPlace place);

/**
 * Give a freshly authored Paint or Fill \a layer the channels it takes part in by default.
 *
 * A description only reaches a channel through a channel record, so a layer with no records is
 * inert and nothing is wired to the Principled BSDF. Every authoring path -- the Outliner's Add,
 * `Material.paint_layers.new`, `MATERIAL_OT_new_layered` -- calls this one function, so the default
 * set is decided in exactly one place and no path can forget it.
 *
 * The set is the material's global channel set (#BKE_paint_layers_channel_set_mask_get): every
 * channel it names gets an enabled record with no map, so a Fill then shows its fill colour in all
 * of them and a Paint contributes nothing until a stroke gives a channel a map. Existing records
 * are left as they are, so the call is idempotent. Folders, corrections and the bake-backed sources
 * (Material, NodeGroup) are left alone; a MESH_MAP row still defaults to Base Color alone.
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
 * Whether \a target is reachable from \a from by following MATERIAL layers' source materials, so
 * setting \a target as a source under \a from would make the bake build a cycle.
 */
bool BKE_paint_layers_material_depends_on(const Material &from, const Material &target);

/**
 * Set the source material a #MA_PAINT_LAYER_SOURCE_MATERIAL \a layer bakes from, maintaining the
 * source's user count.
 *
 * Refuses a null source, \a ma itself, and any source that already depends on \a ma (a cycle that
 * would make the bake recurse forever). A refused call leaves the layer's source untouched.
 *
 * \return false when \a layer is not part of \a ma or the source is refused.
 */
bool BKE_paint_layers_set_material(Material &ma, MaterialPaintLayer *layer, Material *source);

/**
 * Remove \a layer from \a ma, freeing it and its children and corrections recursively.
 *
 * When the active marker names the removed row or any row under it, the active marker is cleared:
 * a marker that resolves to nothing is worse than no selection, and silently moving the selection
 * to a neighbour would hide that the row the user was working on is gone.
 *
 * \return false when \a layer is null or the list owning it cannot be found in \a ma.
 */
bool BKE_paint_layers_remove(Material &ma, MaterialPaintLayer *layer);

/**
 * Find the row of \a ma whose marker is \a marker, searching the whole stack: top-level rows,
 * nested folders and corrections. Never by position, so a row keeps resolving after a reorder.
 *
 * \return the row, or null when no row carries \a marker.
 */
MaterialPaintLayer *BKE_paint_layers_find(Material &ma, const bUUID &marker);

/** Set the active marker of \a ma; nil clears it. */
void BKE_paint_layers_active_set(Material &ma, const bUUID &marker);

/** The active marker of \a ma, nil when nothing is active. */
bUUID BKE_paint_layers_active_get(const Material &ma);

/**
 * Move \a layer, with everything nested under it, relative to \a anchor in \a ma's stack.
 *
 * Same placement rules as #BKE_paint_layers_add: a null anchor appends the row on top of the
 * top-level list, #PaintLayerPlace::Into appends inside the anchor (making it a folder), Above and
 * Below are siblings in the anchor's own list. Markers are untouched, so the active marker keeps
 * resolving.
 *
 * \return false when \a layer is not part of \a ma, when \a anchor is not part of \a ma, or when
 * \a anchor is \a layer itself or nested under it -- a row may never be moved into its own
 * subtree.
 */
bool BKE_paint_layers_move(Material &ma,
                           MaterialPaintLayer *layer,
                           MaterialPaintLayer *anchor,
                           PaintLayerPlace place);

/**
 * Move \a layer to \a index within the list that owns it, keeping its nesting: a row changes
 * position among its siblings, never its parent. The index is clamped to the list.
 *
 * \return false when \a layer is null or not part of \a ma.
 */
bool BKE_paint_layers_reorder(Material &ma, MaterialPaintLayer *layer, int index);

/**
 * Fold \a layers into a new folder row: a row that is a folder by virtue of holding \a layers in
 * its #MaterialPaintLayer::children. The rows keep their markers, their order and their own
 * subtrees; the folder takes the position of the topmost member and is returned.
 *
 * \return the folder, or null when \a layers is empty or its rows do not sit in one and the same
 * list of \a ma's stack (grouping across nesting levels or materials is refused).
 */
MaterialPaintLayer *BKE_paint_layers_group(Material &ma, Span<MaterialPaintLayer *> layers);

/**
 * Lift the rows held by \a folder into the list that owns the folder, at the folder's position and
 * in their order, and free the folder itself. The rows keep their markers and subtrees.
 *
 * \return false when \a folder is null or not part of \a ma. When the active marker named the
 * folder or anything else that is freed with it, it is cleared, as in #BKE_paint_layers_remove.
 */
bool BKE_paint_layers_ungroup(Material &ma, MaterialPaintLayer *folder);

/**
 * Deep-copy the branch of \a layer into a new row placed directly above it, in the list that owns
 * it. Every row of the branch -- children and corrections included -- gets a fresh marker unique
 * within \a ma, its own copy of its #IDProperty group, and copies of the images its channels and
 * mask reference (never the shared image); the custom group stays shared, the way a copied
 * material shares it.
 *
 * \return the copy, or null when \a layer is null or not part of \a ma.
 */
MaterialPaintLayer *BKE_paint_layers_duplicate(Main &bmain,
                                               Material &ma,
                                               MaterialPaintLayer *layer);

/**
 * Whether #BKE_paint_layers_correction_paste can attach a copy of the effect or mask item \a source
 * to \a target: \a target must be a stack row, and a mask item that is not a base needs a base on
 * \a target to sit over.
 */
bool BKE_paint_layers_correction_can_paste(const MaterialPaintLayer &source,
                                           const MaterialPaintLayer &target);

/**
 * Attach a deep copy of the effect or mask item \a source -- which may belong to another material
 * -- to the top of \a target's own corrections. The copy and everything under it gets fresh markers
 * unique within \a ma and copies of the images its channels hold (never the shared image); a source
 * material or node group stays shared with a user of its own.
 *
 * \return the copy, or null when \a target is not a row of \a ma or #BKE_paint_layers_correction_can_paste
 * refuses.
 */
MaterialPaintLayer *BKE_paint_layers_correction_paste(Main &bmain,
                                                      Material &ma,
                                                      const MaterialPaintLayer &source,
                                                      MaterialPaintLayer *target);

/**
 * Give \a layer a new base mask item of strength \a value, inserted first in its mask stack.
 *
 * The item is a constant `MULTIPLY` element, so the row's coverage starts at `F = value`. A
 * Mask-mode stroke turns the item into a map in place.
 *
 * \return the new item, or null when \a layer is null or not part of \a ma.
 */
MaterialPaintLayer *BKE_paint_layers_mask_add(Material &ma, MaterialPaintLayer *layer, float value);

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
 * keeps the map but switches it off. An absent channel is refused -- adding one is an explicit
 * step.
 *
 * \return false when \a layer carries no \a channel record or is not part of \a ma.
 */
bool BKE_paint_layers_channel_set_enabled(Material &ma,
                                          MaterialPaintLayer *layer,
                                          eMaterialPaintChannel channel,
                                          bool enabled);

/**
 * Set the per-channel blend override of \a layer's \a channel.
 *
 * A channel record is created on demand without changing whether the row participates in the
 * channel: its state is left as it was, or #MA_PAINT_LAYER_CHANNEL_ABSENT for a new record. A
 * blend override is a setting of the pair, not a map, so a folder may carry one too.
 *
 * A structural edit: the generated tree is marked stale. \a blend is an #eMaterialPaintLayerBlend,
 * or `-1` to inherit the row's blend.
 */
bool BKE_paint_layers_channel_blend_set(Material &ma,
                                        MaterialPaintLayer &layer,
                                        int channel,
                                        int blend);

/**
 * Set the per-channel opacity multiplier of \a layer's \a channel. A channel record is created on
 * demand the same way #BKE_paint_layers_channel_blend_set does.
 *
 * Value-only: the generated tree's per (row, channel) opacity input is brought current through
 * #BKE_paint_layers_values_sync, so an animated opacity never rebuilds the topology.
 */
bool BKE_paint_layers_channel_opacity_set(Material &ma,
                                          MaterialPaintLayer &layer,
                                          int channel,
                                          float opacity);

/**
 * Change the source of \a layer between Image and Constant, converting what the two disagree on:
 * Image keeps its channels as future maps, Constant gives them up for its constant color (a
 * painted map is not a constant), and the constant color carries into Image as the starting point
 * of the first stroke's map.
 *
 * Refused for anything else: a Material/NodeGroup/Stack layer is created, not converted, a
 * custom-group layer likewise, and a correction changes its source through
 * #BKE_paint_layers_correction_source_set instead, never through this one.
 *
 * \return false when \a layer is null, not part of \a ma, not a stack Layer (#PaintLayerRole), or
 * \a source is not Image or Constant.
 */
bool BKE_paint_layers_source_change(Material &ma, MaterialPaintLayer *layer, int8_t source);

/**
 * Add a correction row under \a owner: a row carrying \a role (Effect or MaskItem) and \a source
 * (Image or Constant), linked into the owner's #MaterialPaintLayer::effects or #mask_stack list to
 * match. \a name may be null, in which case "Correction" is used. When \a after is a row of the
 * list the new row joins, it is linked right behind it instead of at the end of the list.
 *
 * \return the new row, or null when \a owner is null or not part of \a ma, or \a role or \a source
 * is out of range.
 */
MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    int role,
                                                    int source,
                                                    const char *name,
                                                    MaterialPaintLayer *after = nullptr);

/**
 * Set the role (#PaintLayerRole, Effect or MaskItem) of \a correction, moving it between its
 * owner's #MaterialPaintLayer::effects and #mask_stack lists to match.
 *
 * \return false when \a correction is null, is not itself a correction (a stack Layer has no role
 * to set), has no owner in \a ma, or \a role is not Effect or MaskItem -- in particular, Layer is
 * always refused: a correction never becomes a stack row through this function.
 */
bool BKE_paint_layers_role_set(Material &ma, MaterialPaintLayer *correction, int role);

/**
 * Set the source (Image or Constant) of \a correction; the correction-scoped counterpart of
 * #BKE_paint_layers_source_change, restricted to a row that is not a stack Layer.
 *
 * \return false when \a correction is null, is a stack Layer, has no owner in \a ma, or \a source
 * is not Image or Constant.
 */
bool BKE_paint_layers_correction_source_set(Material &ma, MaterialPaintLayer *correction, int source);

/**
 * Set the #eMaterialMeshMapType of \a layer, which must be a #MA_PAINT_LAYER_SOURCE_MESH_MAP row.
 *
 * A source-typed property like the source itself: it names which geometry map the row reads, not a
 * value. Marks the generated tree stale (the map a row reads is topology).
 *
 * \return false when \a layer is null, is not part of \a ma, is not a MESH_MAP row, or \a type is
 * out of range.
 */
bool BKE_paint_layers_mesh_map_type_set(Material &ma, MaterialPaintLayer *layer, int8_t type);

/** Set the blend of \a layer; see #eMaterialPaintLayerBlend. */
bool BKE_paint_layers_set_blend(Material &ma,
                                MaterialPaintLayer *layer,
                                eMaterialPaintLayerBlend blend);

/** Set the opacity of \a layer. */
bool BKE_paint_layers_set_opacity(Material &ma, MaterialPaintLayer *layer, float opacity);

/** Set the fill color of \a layer, the constant a Fill layer shows. */
bool BKE_paint_layers_set_fill_color(Material &ma, MaterialPaintLayer *layer, const float color[4]);

/** Set the display color tag of \a layer; interpreted by the UI only. */
bool BKE_paint_layers_set_color_tag(Material &ma, MaterialPaintLayer *layer, int8_t color_tag);

/** Rename \a layer. */
bool BKE_paint_layers_rename(Material &ma, MaterialPaintLayer *layer, const char *name);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake
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

/** Drop \a ma's runtime subscription state (free, copy, file load). */
void BKE_paint_layers_bake_runtime_free(Material &ma);

/**
 * Move the subscription entry of \a src to \a dst when it is \a src that owns it. Called from the
 * memfile-undo preserve, where the re-read ID (\a dst) keeps the old ID's `session_uid`: without the
 * move, releasing the old ID would drop the re-read ID's subscription.
 */
void BKE_paint_layers_bake_runtime_owner_transfer(Material &dst, Material &src);

/**
 * Whether \a layer's bake is heavy enough to leave the main thread: its subtree would add more than
 * #PAINT_LAYERS_AUTO_BAKE_NODES nodes, or its declared map size is at least
 * #PAINT_LAYERS_HEAVY_BAKE_SIZE. A light row is baked synchronously at the K-1 point; a heavy one
 * is queued to a wmJob so the UI keeps running.
 */
bool BKE_paint_layers_bake_is_heavy(const Material &ma, const MaterialPaintLayer &layer);

/**
 * How many folders enclose \a layer, counting \a layer itself when it is a folder: a layer in the
 * stack root is 0, a folder in the stack root is 1, a folder nested in it is 2. A leaf correction
 * has its owner's level; a Stack correction is itself a folder and counts itself like one (a Stack
 * correction on a root layer is 1). \return -1 when \a layer is not in \a ma.
 */
int BKE_paint_layers_folder_level(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether the AUTO bake mode may bake \a layer: only folders nested inside another folder (level 2
 * and deeper). A folder in the stack root stays live. A manual ALWAYS is the user's choice and is
 * not gated by this.
 */
bool BKE_paint_layers_folder_auto_bake_allowed(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Set every warm slot the plan entitles a row to present again after the user used it, and tag the
 * material for regeneration. Meant for the idle tick after an edit: the graph change it causes is
 * the background recompile that keeps the next add instant. \return whether anything changed.
 */
bool BKE_paint_layers_warm_replenish(Material &ma);

/** The map side at or above which a bake counts as heavy regardless of the subtree's weight. */
constexpr int PAINT_LAYERS_HEAVY_BAKE_SIZE = 2048;

/** Whether any heavy, not-yet-valid baked row of \a ma waits for a worker. */
bool BKE_paint_layers_bake_heavy_pending(const Material &ma);

/**
 * Idle time a correction visibility toggle waits before its row is re-baked. Someone looking for
 * the right correction flips several in a row; each flip moves the row's hash, so a shorter wait
 * would start a heavy bake (and the graph rebuild that follows it) for every flip.
 */
constexpr double PAINT_LAYERS_BAKE_EDIT_QUIET_SECONDS = 1.5;

/**
 * Time a row may stay hidden before the idle tick drops it from the graph. Under it the toggle is a
 * value edit: the row keeps its nodes and only its factor goes to zero, so flipping a row off and
 * back on costs neither a rebuild nor an EEVEE compile. Past it the row leaves the graph and coming
 * back is the one rebuild (and possible re-bake) that its return needs.
 */
constexpr double PAINT_LAYERS_COLD_TIER_SECONDS = 60.0;

/** Restart \a ma's long bake debounce window; call after a visibility toggle. */
void BKE_paint_layers_bake_debounce_extend(Material &ma);

/**
 * The debounce delay to arm for \a ma: \a base_seconds, or the rest of the window opened by
 * #BKE_paint_layers_bake_debounce_extend when that is longer. Also reports the rest to the log.
 */
double BKE_paint_layers_bake_debounce_seconds(const Material &ma, double base_seconds);

/**
 * Whether \a layer must stay live right now rather than be baked: it is the active row, or
 * an ancestor of it, so the user is editing inside it and a bake would fight the edit.
 *
 * Both bake planners ask this one question -- the CPU one at the K-1 point and the editor
 * one for Material rows -- so "the row the user is working in" means the same thing on both
 * sides. Says nothing about weight or validity; those stay each planner's own decision.
 */
bool BKE_paint_layers_bake_row_is_deferred(const Material &ma,
                                           const MaterialPaintLayer &layer);

/**
 * Whether any channel of \a layer is currently shown from its source rather than from a
 * baked map -- a constant or a map. The editor uses it to know that an edit to that source
 * has to reach this layered material.
 */
bool BKE_paint_layers_material_lives_from_source(const Material &ma,
                                                 const MaterialPaintLayer &layer);

/** How a Material row is shown right now. */
enum class PaintLayerMaterialMode : int8_t {
  /** Its baked maps: the row is not live, or the source cannot be expressed. */
  Baked = 0,
  /** Its source's constants and plainly mapped textures, channel by channel. */
  Hybrid,
  /** Its source's whole graph, through the wrapper group. */
  SourceGroup,
};

/**
 * The mode \a layer is in right now. One answer for the generator and the CPU compositor:
 * they must never disagree about what a row shows.
 *
 * Hybrid is preferred when every channel the row needs resolves to a constant or a plainly
 * mapped texture, because the CPU can reproduce it and the Image Editor then matches the
 * viewport. Anything else that is live goes through the wrapper group, which the CPU cannot
 * evaluate -- there the Image Editor keeps showing the last bake.
 */
PaintLayerMaterialMode BKE_paint_layers_material_mode(const Material &ma,
                                                      const MaterialPaintLayer &layer,
                                                      const PaintLayersRegenCache *cache = nullptr);

/**
 * Runtime claim on a map that a bake job is rendering, keyed by the image's session UID and counted
 * so an overlapping job on the same map keeps it claimed. The editor's job code owns the calls.
 */
void BKE_paint_layers_bake_image_pending_add(uint32_t image_session_uid);
void BKE_paint_layers_bake_image_pending_remove(uint32_t image_session_uid);

/**
 * Drop the runtime sampler state keyed by \a ma's `session_uid`. Called when the material is freed so
 * a reused uid cannot inherit another material's forced set, and no entry outlives its owner.
 */
void BKE_paint_layers_sampler_state_free(const Material &ma);

/**
 * Move the sampler runtime entry (forced-bake rows and the cleanup owner) of \a src to \a dst when
 * it is \a src that owns it. Called from the memfile-undo preserve beside
 * #BKE_paint_layers_bake_runtime_owner_transfer.
 */
void BKE_paint_layers_sampler_state_owner_transfer(Material &dst, Material &src);

/**
 * Whether \a image is a baked map of a row that must stay live right now: the active row of
 * some layered material, or an ancestor of it.
 *
 * The editor's image-rebake walk finds maps by their bake link to a source material and
 * knows nothing about which row holds them, so this is how it leaves the row the user is
 * editing alone. Walks every layered material, which is cheap: it runs only for materials
 * that actually have maps baked from the edited source.
 */
bool BKE_paint_layers_bake_image_is_deferred(const Main &bmain, const Image &image);

/**
 * Whether \a source is read live by a Material row right now: some layered material has a
 * `MATERIAL` row whose source is \a source, that row is deferred (the active row or an ancestor of
 * it), and its mode is not #PaintLayerMaterialMode::Baked.
 *
 * While this holds, the user is editing the source through the live row, so the source's own
 * automatic re-bakes have to wait until the active marker leaves the row. Localized and evaluated
 * copies are skipped: they share the original's description.
 */
bool BKE_paint_layers_source_material_is_live(const Main &bmain, const Material &source);

/**
 * Append every layered material of \a bmain that has at least one `MATERIAL` row -- or Effect
 * correction -- whose source is \a source, once each.
 *
 * A bake job of \a source is keyed on the source material itself, so a layered material reading
 * it through a row has no `wmJob` of its own to announce the render. This is the list whose
 * `#MA_PAINT_LAYERS_BAKE_SCHEDULED` mark the editor stamps before starting such a job; the settle
 * path (#paint_layers_bake_jobs_in_flight) finds the same rows to clear it again.
 *
 * Localized and evaluated copies are skipped: they share the original's description.
 */
void BKE_paint_layers_source_material_consumers(const Main &bmain,
                                                const Material &source,
                                                Vector<Material *> &r_consumers);

/**
 * A heavy-bake job: the description is localized on the main thread at creation, the pixels are
 * computed from the copy on the worker (#BKE_paint_layers_bake_job_compute), and the maps are
 * written back to the live material on the main thread (#BKE_paint_layers_bake_job_commit).
 *
 * The copy is what makes it safe: the worker never reads a material the main thread is editing.
 * The commit re-finds the live material by session UID and every row by marker, so a job whose
 * material or rows went away while it ran is dropped instead of writing into freed memory.
 */
struct PaintLayersBakeJob;

/** Collect \a ma's heavy pending rows and localize its description; null when there is none. */
PaintLayersBakeJob *BKE_paint_layers_bake_job_create(Main &bmain, Material &ma);

/**
 * Render every collected row from the localized copy. Runs on the worker thread.
 *
 * \param report_progress: called after each row/channel pair finishes, with the fraction (0..1) of
 * the job's total row/channel pairs done so far; left empty (the default) by every caller that has
 * nothing to report progress to, such as a test.
 */
void BKE_paint_layers_bake_job_compute(PaintLayersBakeJob &job,
                                       FunctionRef<void(float progress)> report_progress = {});

/**
 * Write the computed maps back to the live material and mark the tree stale. Runs on the main
 * thread; returns whether anything was committed.
 */
bool BKE_paint_layers_bake_job_commit(PaintLayersBakeJob &job);

/** Free \a job and its localized copy. */
void BKE_paint_layers_bake_job_free(PaintLayersBakeJob &job);

/**
 * A "Use Row Result" job: render one row (\a source_marker) of the layered \a ma for every channel
 * it takes part in, and add the results as channel maps of another row (\a target_marker).
 *
 * Same split as #PaintLayersBakeJob -- the description is localized at creation, the pixels are
 * computed on the worker and the maps are created and written on the main thread -- because a heavy
 * row at 2048+ pixels is too slow to render inline in the operator.
 */
struct PaintLayersRowResultJob;

/** Localize \a ma and remember the two rows; null when either is missing or not layered. */
PaintLayersRowResultJob *BKE_paint_layers_row_result_job_create(Main &bmain,
                                                                Material &ma,
                                                                const bUUID &source_marker,
                                                                const bUUID &target_marker,
                                                                int size);

/** Render \a source_marker's channels from the localized copy. Runs on the worker thread. */
void BKE_paint_layers_row_result_job_compute(PaintLayersRowResultJob &job);

/**
 * Create the target row's maps from the computed buffers and link them. Runs on the main thread;
 * returns whether anything was committed.
 */
bool BKE_paint_layers_row_result_job_commit(PaintLayersRowResultJob &job);

/** Free \a job and its localized copy. */
void BKE_paint_layers_row_result_job_free(PaintLayersRowResultJob &job);

/** Whether \a ma has a Material row left behind by the active row moving. */
bool BKE_paint_layers_material_bake_due_get(const Material &ma);
/** Clear \a ma's due mark; the editor planner calls this once it has run. */
void BKE_paint_layers_material_bake_due_clear(Material &ma);

/**
 * Whether the editor has a bake outstanding for \a ma right now: a debounce timer armed after an
 * edit, or a bake job started but not committed yet. BKE cannot see `wmJob` state on its own, so
 * this is the editor's hand-off -- set and cleared only by `ED_` scheduling code, never derived
 * here.
 */
bool BKE_paint_layers_bake_scheduled_get(const Material &ma);
/** Set or clear \a ma's outstanding-bake mark; see #BKE_paint_layers_bake_scheduled_get. */
void BKE_paint_layers_bake_scheduled_set(Material &ma, bool scheduled);

/**
 * Whether \a ma's paint-layer result is not the one its current description would produce right
 * now: some row's stored bake does not match #BKE_paint_layers_bake_is_valid, a heavy bake is
 * queued but not yet run (#BKE_paint_layers_bake_heavy_pending), or the editor has a bake
 * outstanding for it (#BKE_paint_layers_bake_scheduled_get).
 *
 * This is the BKE half of "is the result fresh": it has no visibility into a light bake job
 * already running through `wmJob` that has not yet set the scheduled mark, so a caller that can
 * see window-manager state should prefer `ED_paint_layers_stale_or_pending`, which also checks
 * that.
 */
bool BKE_paint_layers_is_stale(const Material &ma);

/**
 * What a caller wanting a paint-layer bake *result* -- a bake/export/"Apply" operator, or the RNA
 * `Material.paint_layers.composite()` this mirrors -- should do about a stale one, from three
 * things it already knows about itself: whether the result it read is stale
 * (#BKE_paint_layers_is_stale, or `ED_paint_layers_stale_or_pending` where a #wmWindowManager is at
 * hand and the `wmJob`-only window matters), whether it was reached through `invoke` (a person, who
 * can wait a moment) rather than `exec`/an RNA function (a script, which cannot), and whether it is
 * running headless at all (\a headless, `wm == nullptr`).
 *
 * Headless always resolves to #Proceed regardless of \a stale: #material_changed's headless branch
 * already runs every bake synchronously before this could ever be asked, so \a stale is normally
 * false there anyway, and there is no timer, no `wmJob`, and no modal loop for a background script
 * to ever become fresh through -- refusing it would only ever be a false refusal, never a real
 * wait. This is the one rule #BKE_paint_layers_is_stale's own doc-comment leaves to the caller: it
 * has no idea whether it is being asked from a script or a person, headless or not.
 *
 * A person (`invoke`) gets #Wait: go modal on a short `wmTimer`, matching the accepted UI decision
 * ("Waiting for paint layers bake..."), and re-ask once it ticks. A script (`exec`, or the RNA
 * function) gets #Refuse: report the error named in `Material.paint_layers_is_stale`'s own
 * doc-comment and stop, since a script cannot go modal at all.
 *
 * Pure -- three booleans in, one of three outcomes out -- so the one behavioral rule every result
 * consumer must share (headless first, then invoke-vs-exec) lives in one place, shared by the RNA
 * layer and the editor wrappers rather than duplicated.
 */
enum class PaintLayersBakeGateAction {
  /** Nothing is stale, or nothing can be done about it here (headless): go ahead now. */
  Proceed,
  /** `invoke` reached a stale result: go modal and re-ask once the bake has had time to run. */
  Wait,
  /** `exec`/RNA reached a stale result and cannot go modal: report and stop. */
  Refuse,
};

PaintLayersBakeGateAction BKE_paint_layers_bake_gate_decide(bool stale,
                                                             bool is_invoke,
                                                             bool headless);

/** Drop \a layer's bake cache and mark the generated tree stale. */
bool BKE_paint_layers_bake_clear(Material &ma, MaterialPaintLayer &layer);

/**
 * Stamp \a layer's bake hash from its description and start the partial-update subscription, after
 * its maps were filled through #BKE_paint_layers_bake_set_map. This is what makes
 * #BKE_paint_layers_bake_substitute take the row.
 */
void BKE_paint_layers_bake_finalize(Material &ma, MaterialPaintLayer &layer);

/**
 * Store the result of an external material bake on \a layer: \a channels[i] is the
 * #eMaterialPaintChannel of \a images[i], as a material bake hands them over.
 *
 * An #PAINT_MATERIAL_CHANNEL_ALPHA map becomes the row's coverage -- the source's transparency is
 * what limits it over the stack -- and is not kept as a channel of its own. Without one the
 * coverage is an opaque white map, so a source that does not feed alpha fully covers. The images
 * must already belong to \a bmain.
 *
 * This does NOT finalise the bake: a caller handing over target images before their render has
 * actually landed pixels (the async material-images job pattern) must call
 * #BKE_paint_layers_bake_finalize itself only once that render is known to have succeeded, or a
 * cancelled/failed job leaves the row stamped valid over blank or stale maps.
 *
 * \a detach_channels are channels the bake deliberately skipped because the source shows them as a
 * constant; a map left from an earlier bake is detached so the row shows the live value.
 */
void BKE_paint_layers_material_bake_apply(Main &bmain,
                                          Material &ma,
                                          MaterialPaintLayer &layer,
                                          int size,
                                          Span<int> channels,
                                          Span<Image *> images,
                                          Span<int> detach_channels = {});

/**
 * Store the result of a Custom group's GPU bake on \a layer.
 *
 * \a color_buffers[i] holds \a channels[i]'s straight scene-linear RGB with the group's coverage in
 * its alpha; \a coverage_buffer (may be null) holds the group's own coverage output in its alpha,
 * overriding the per-channel one. The row's service maps are created or reused, written, hashed and
 * subscribed, so #BKE_paint_layers_bake_substitute starts taking the row. Runs on the main thread;
 * the buffers are read, not owned.
 */
void BKE_paint_layers_custom_bake_apply(Main &bmain,
                                        Material &ma,
                                        MaterialPaintLayer &layer,
                                        int size,
                                        Span<int> channels,
                                        Span<const ImBuf *> color_buffers,
                                        const ImBuf *coverage_buffer);

/**
 * Hash of the subtree and parameters \a layer's bake depends on, the validity key. Two 32-bit
 * words, low first.
 *
 * Structure and parameters only -- source, role, blend, flags, opacity, fill colour, channel/mask
 * maps by `session_uid`, mask value, effects, mask items, children order, the source material, a
 * Custom group, its IDProperty inputs, and the bake size. Pixel edits are not visible here; they
 * arrive
 * through the partial-update subscription and settle as a re-bake of the changed rectangle.
 */
void BKE_paint_layers_bake_hash(const MaterialPaintLayer &layer, uint32_t r_hash[2]);

/**
 * Like #BKE_paint_layers_bake_hash, but folds in the state that lives on \a ma rather than the row:
 * the identity of the MESH_MAP atlas a MESH_MAP row reads. #BKE_paint_layers_bake_is_valid and the
 * bake writers use this overload so pointing a mesh map slot at another Image invalidates the row's
 * bake while a pixel edit (which does not change the session UID) does not.
 */
void BKE_paint_layers_bake_hash(const Material &ma,
                                const MaterialPaintLayer &layer,
                                uint32_t r_hash[2]);

/**
 * Whether \a layer's stored bake can stand in for its live subtree right now.
 *
 * True when a bake exists, its stored hash matches #BKE_paint_layers_bake_hash, and the runtime
 * partial-update subscription has reported no change to the source maps since the bake. The
 * subscription is runtime-only: a freshly loaded file has none, so the first check re-bakes.
 */
bool BKE_paint_layers_bake_is_valid(const Material &ma, const MaterialPaintLayer &layer);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Custom layer helpers and remaining edits
 * \{ */

/** Set whether \a layer takes part in the stack. */
bool BKE_paint_layers_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled);

/* -------------------------------------------------------------------- */
/** \name Row UV mapping (stage 1: Fill layer / Fill correction / Fill mask)
 * \{ */

/** Lower bound of the mapping scale magnitude; a zero axis reads back as one. */
constexpr float PAINT_LAYER_MAPPING_SCALE_MIN = 1e-3f;

/**
 * Normalized mapping scale of one axis: zero reads back as one, the magnitude
 * never drops below #PAINT_LAYER_MAPPING_SCALE_MIN. The one helper RNA,
 * value-sync and the CPU compositor share, so an old file's zeroes cannot
 * disagree between them.
 */
float BKE_paint_layers_mapping_scale_normalize(float scale);

/**
 * Whether a Fill row reads a user-assigned map in \a channel instead of its
 * constant: a Layer or an Effect with a Constant source whose channel record
 * carries a live image. The generator, the CPU composite and the topology hash
 * all ask this, so they cannot disagree about it.
 */
bool BKE_paint_layers_fill_reads_map(const Material &ma,
                                      const MaterialPaintLayer &row,
                                      int channel);

/**
 * Whether \a layer may carry UV mapping: it reads at least one map through
 * #BKE_paint_layers_fill_reads_map (Layer/Effect) or its mask image
 * (Mask Item), or it is a Layer row over a source material. Mesh-Map atlases
 * (forced Extend), Material corrections/masks and Node-Group/Stack rows never
 * support it. The one predicate UI, RNA, the graph and the CPU share; UI never
 * inspects channel records itself.
 */
bool BKE_paint_layers_mapping_supported(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether \a layer's mapping is actually applied right now: enabled, supported, and -- a Material
 * row only -- in a mode that builds it (Hybrid's live textures or the SourceGroup wrapper). A
 * Baked Material row (forced bake) ignores its mapping (a BAKE_NEVER row stays live), so this is
 * the predicate the topology hash, the row interface, the warm spares and the CPU stack build by;
 * the values never move it, an offset/scale/rotation drag is value-only.
 *
 * \param cache: the regeneration cache to read a row's mode through, or null to compute it.
 */
bool BKE_paint_layers_mapping_applies(const Material &ma,
                                      const MaterialPaintLayer &layer,
                                      const PaintLayersRegenCache *cache = nullptr);

/**
 * Whether \a layer must stay live (never substituted by a bake nor planned for one): its own
 * mapping applies, or that of any effect, mask item or child inside it. The mapping values are
 * not part of the bake hash, so the row is the only place they are honored per drag tick.
 */
bool BKE_paint_layers_mapping_blocks_bake(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const PaintLayersRegenCache *cache = nullptr);

/** Whether \a layer's mapping is enabled. */
bool BKE_paint_layers_mapping_enabled_get(const MaterialPaintLayer &layer);

/**
 * Set whether \a layer's mapping is enabled. Refused when
 * #BKE_paint_layers_mapping_supported reports false. Structural: the generated
 * tree gains or loses the row's Mapping node.
 */
bool BKE_paint_layers_mapping_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled);

/**
 * Set the mapping offset of \a layer. Value-only: carried on the row group's
 * inputs through #BKE_paint_layers_values_sync, never a rebuild.
 */
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

/** Whether the scale axes of \a layer move together (the Lock toggle). */
bool BKE_paint_layers_mapping_scale_lock_get(const MaterialPaintLayer &layer);

/**
 * Lock or unlock the scale axes of \a layer. While locked, #BKE_paint_layers_mapping_set_scale
 * gives both axes the value of the one that changed; locking joins them at the X value at once.
 * Value-only, never a rebuild.
 */
bool BKE_paint_layers_mapping_set_scale_lock(Material &ma, MaterialPaintLayer *layer, bool locked);

/**
 * Set the mapping rotation of \a layer, in radians around Z. Value-only.
 *
 * The rotation changes where the row's maps are read. A Normal map's tangent-space vectors are
 * rotated by it too (and flipped by the sign of the scale), so the lighting follows the pattern.
 */
bool BKE_paint_layers_mapping_set_rotation(Material &ma,
                                            MaterialPaintLayer *layer,
                                            float rotation);

/** \} */

/**
 * Set the custom node group of \a layer. The old group loses the layer's user, the new one gains
 * it; user counts are what keeps a group alive, so they are maintained here rather than by the
 * caller.
 *
 * \return false when \a layer is null or not part of \a ma.
 */
bool BKE_paint_layers_set_custom_group(Material &ma,
                                       MaterialPaintLayer *layer,
                                       bNodeTree *group);

/**
 * Create a Custom row with a fresh group template: `BELOW:BASE_COLOR` and `UV` inputs,
 * `COLOR:BASE_COLOR` and `COVERAGE` outputs, and a default body that passes the row below through.
 * The group belongs to the row alone. The row is placed by \a anchor/\a place and made active.
 *
 * \return the new row, or null when the group could not be created.
 */
MaterialPaintLayer *BKE_paint_layers_custom_layer_add(Main &bmain,
                                                      Material &ma,
                                                      const char *name,
                                                      MaterialPaintLayer *anchor,
                                                      PaintLayerPlace place);

/**
 * Add a `BELOW:<CHANNEL>` / `COLOR:<CHANNEL>` pair with roles to \a layer's Custom group, so the
 * layer declares one more channel. Creates the group if it has none. Refused when the channel is
 * already declared.
 */
bool BKE_paint_layers_custom_channel_add(Main &bmain,
                                         Material &ma,
                                         MaterialPaintLayer &layer,
                                         eMaterialPaintChannel channel);

/** \} */

}  // namespace blender

