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

#include "BLI_map.hh"
#include "BLI_vector.hh"
#include "DNA_uuid_types.h"

namespace blender {

struct Image;
struct ImageUser;
struct ImBuf;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerBake;
struct MaterialPaintLayerChannel;
struct PaintLayersRegenCache;
struct bNodeTree;
enum eMaterialPaintChannel : int8_t;
enum eMaterialPaintLayerKind : int8_t;
enum eMaterialPaintLayerBlend : int8_t;

template<typename T> class Span;

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
 * Whether \a layer is a folder: a row that holds its stack in #MaterialPaintLayer::children and
 * takes part in a channel through them alone.
 *
 * This is the single test for folder-ness everywhere -- the generator, the CPU compositor, the
 * channel rules and the UI -- so a row can never be a folder on one side and a leaf on another. It
 * reads #eMaterialPaintLayerKind, never a non-empty #children list: an emptied folder stays a
 * folder, and a malformed leaf that somehow has children is still a leaf.
 */
bool BKE_paint_layers_is_folder(const MaterialPaintLayer &layer);

/**
 * Whether \a folder can be inlined into its parent's chain instead of isolating its children.
 *
 * A Pass Through folder changes nothing its children do, so the generator expands it in place and
 * the CPU compositor flattens it: the same rows, the same order, no wrapper. The mode is a pure
 * function of the description -- no mask item, no content correction, Mix blend and opacity one on
 * every channel (per-channel overrides included) and no valid bake standing in -- so the generator
 * and the compositor can never disagree about it. Visibility is deliberately not part of the test:
 * a hidden Pass Through folder scales its children's factor to zero as a value edit.
 */
bool BKE_paint_layers_folder_is_pass_through(const Material &ma,
                                             const MaterialPaintLayer &folder);

/**
 * Where a row reads its values from, the description-level view of a row's source.
 *
 * It is derived from #eMaterialPaintLayerKind (and, for a correction, from
 * #eMaterialPaintLayerCorrectionEffect) so callers ask what a row *is* rather than which stored
 * fields happen to encode it. Phase 2.2 replaces those fields with a dedicated source, and this
 * mapping is the one place that changes.
 */
enum class PaintLayerSourceType : int8_t {
  /** A painted map; a Paint row with no map yet still reads as Image (its flat value is the
   * starting point of its first stroke, not a constant source). */
  Image = 0,
  /** A flat constant: a Fill row, or a Fill effect. */
  Constant,
  /** Another material's channels, baked. */
  Material,
  /** A user's node group. */
  NodeGroup,
  /** A nested stack: a folder. */
  Stack,
};

/**
 * The #PaintLayerSourceType \a layer reads from.
 *
 * Paint -> Image, Fill -> Constant, Material -> Material, Custom -> NodeGroup, Folder -> Stack,
 * and a Correction answers by its effect: Paint effect -> Image, Fill effect -> Constant.
 */
PaintLayerSourceType BKE_paint_layers_source_type(const MaterialPaintLayer &layer);

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

/**
 * The #PaintLayerRole \a layer takes: Effect for a Correction in the content section, MaskItem for
 * one in the mask section, and Layer for every other row.
 */
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
 * Static description of one #eMaterialPaintLayerKind: the per-kind switches that used to be
 * `ELEM(kind, ...)` checks scattered across the generator, the CPU compositor and the bake.
 *
 * One table instead of many, so adding a kind is one edit here plus its branches, and the
 * Outliner's add-kinds list, the generator and the bake all read the same answer.
 */
struct PaintLayerKindInfo {
  int kind;
  /** Stable identifier shared with the Outliner's add-kinds and the Python API. */
  const char *identifier;
  /** Untranslated UI name. */
  const char *ui_name;
  /** The row holds its stack in #MaterialPaintLayer::children and takes part through it alone. */
  bool is_folder;
  /** The row paints with #MaterialPaintLayer::fill_color rather than a map or value. */
  bool uses_fill_color;
  /**
   * The CPU compositor cannot evaluate the row: an editor bake fills its maps and the
   * generator/CPU substitute them. MATERIAL bakes its source through the material bake; CUSTOM
   * is rendered through EEVEE/AOV.
   */
  bool needs_external_bake;
};

/** The descriptor for \a kind; an unknown kind reads back as Paint. */
const PaintLayerKindInfo &BKE_paint_layers_kind_info(int kind);

/**
 * Convert a Fill row to Paint, carrying its constant into every channel record first, so a stroke
 * can later give one channel a map while the others stay flat: kind -> Paint, every existing
 * record's value set to the row's fill colour.
 *
 * \param r_fill: receives the row's fill colour (scene linear), valid after the call.
 * \return true when the row was a Fill and was converted; false when it was already Paint.
 */
bool BKE_paint_layers_fill_to_paint(Material &ma, MaterialPaintLayer &layer, float r_fill[4]);

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

/** Whether \a marker names \a layer or anything nested under it (children, effects,
 * mask stack). */
bool BKE_paint_layers_subtree_contains(const MaterialPaintLayer &layer, const bUUID &marker);

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
 * The factor \a layer blends by before any per-pixel coverage: its own opacity, zero when it is
 * disabled, and -- when its mask is a constant rather than a map -- the mask's value folded in.
 *
 * The generator, the value sync and the CPU composite all read this one helper, so a constant mask
 * and a switched-off row cannot mean different things on the two sides. A mask that carries a map
 * is not folded here: its per-pixel alpha multiplies the factor at render and composite time.
 */
float BKE_paint_layers_effective_opacity(const MaterialPaintLayer &layer);

/**
 * The blend \a layer's \a channel blends by: the channel record's override, or the row's
 * #MaterialPaintLayer::blend when the channel has no record or records `-1` (inherit).
 *
 * The generator, the CPU compositor and the UI all read this one helper, so a per-channel blend
 * cannot mean two different things.
 */
int BKE_paint_layers_channel_blend_effective(const MaterialPaintLayer &layer, int channel);

/**
 * The factor \a layer's \a channel blends by: the row's effective opacity
 * (#BKE_paint_layers_effective_opacity, which folds enabled and a constant mask) times the
 * channel record's #MaterialPaintLayerChannel::opacity, or the row's effective opacity alone when
 * the channel has no record.
 */
float BKE_paint_layers_channel_opacity_effective(const MaterialPaintLayer &layer, int channel);

/**
 * The flat colour \a correction contributes to \a channel when its effect is Fill: the correction's
 * own fill colour, or -- for a Paint correction with no map in this channel -- the channel's flat
 * value. The generator and the CPU composite read this one helper, so a constant correction cannot
 * mean two different colours.
 */
void BKE_paint_layers_correction_constant(const MaterialPaintLayer &correction,
                                          eMaterialPaintChannel channel,
                                          float r_color[4]);

/**
 * Decode one straight RGBA sample of \a image out of its stored colorspace and into scene linear.
 *
 * This is the per-element form of the conversion the Image Texture node performs, kept for the
 * cross-test interpreter and for unit-testing the round trip; compositing decodes whole buffers at
 * once (see the CPU composite), never pixel by pixel. A data (Non-Color) or already scene-linear
 * colorspace is the identity.
 */
void BKE_paint_layers_sample_to_linear(const Image &image, float rgba[4]);

/** The inverse of #BKE_paint_layers_sample_to_linear: encode a scene-linear RGBA sample. */
void BKE_paint_layers_sample_from_linear(const Image &image, float rgba[4]);

/**
 * Decode a description constant -- a Fill colour or a channel's flat value -- into scene linear.
 *
 * The description stores these as RNA `PROP_COLOR` values, which are already scene linear, so this
 * is the identity today and the one place that would change if a gamma-stored constant appeared.
 * The generator, the value sync and the CPU composite all read it, so the two sides cannot disagree
 * about what a constant means.
 */
void BKE_paint_layers_constant_to_linear(eMaterialPaintChannel channel,
                                         const float rgba[4],
                                         float r_linear[4]);

/**
 * The `MA_RAMP_*` code a description blend stands for -- the one table the generator's Mix nodes
 * and the CPU's `ramp_blend` both read, so a new blend cannot mean two different things.
 *
 * `MA_PAINT_LAYER_BLEND_NORMAL_COMBINE` is not a Mix mode and has no ramp code; it answers
 * `MA_RAMP_BLEND`, and the Normal channel never reaches this function with it.
 */
int BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend blend);

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

/** The identifier a Custom role uses for \a channel, e.g. `"BASE_COLOR"`, or null. */
const char *BKE_paint_layers_custom_channel_identifier(int channel);

/**
 * Give every role-less input of a Custom layer's group a stored value in
 * `MaterialPaintLayer::properties`, keyed by the socket identifier, so the UI has a value to edit
 * and the bake has something to instantiate with. Existing values are left alone.
 */
void BKE_paint_layers_custom_properties_sync(Material &ma);

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
 * refused rather than promoted -- a kind is never changed implicitly, and promoting a Paint or Fill
 * layer would make its maps ignored. A UI that drops "into" a plain row groups instead (see
 * #BKE_paint_layers_group). The row is
 * given a fresh #bUUID marker unique within \a ma, and the usual defaults: the \a kind, Mix blend,
 * enabled, full opacity.
 *
 * Sets #MA_PAINT_LAYERED on \a ma -- a material that owns a description is a layered material, and
 * the old graph-as-truth path refuses it from then on -- and marks the generated tree stale; see
 * the file comment.
 *
 * \return the new row, or null when \a anchor is non-null but is not part of \a ma's stack. No
 * other call may place a row under a foreign anchor, so all three places validate it first.
 */
MaterialPaintLayer *BKE_paint_layers_add(Material &ma,
                                         eMaterialPaintLayerKind kind,
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
 * The default set is the channels that reach a Principled socket and can carry a map, minus Normal
 * and Height: Base Color, Metallic, Roughness, Specular, Alpha and Emission. Each gets an enabled
 * record with no map: a Fill then shows its fill colour in all of them, a Paint contributes
 * nothing until a stroke gives a channel a map. Existing records are left as they are, so the call
 * is idempotent. Folders, corrections and the bake-backed kinds (Material, Custom) are left alone.
 */
void BKE_paint_layers_default_channels_apply(Material &ma, MaterialPaintLayer &layer);

/**
 * The value a new channel record of \a layer starts from: the Principled input's own default
 * (Metallic 0, Roughness 0.5, Specular IOR Level 0.5, ...), or the table fallback for a material
 * with no Principled. Scalar channels come back as a grey RGBA with alpha one, since the generated
 * chain and the CPU read the channel's mean.
 *
 * A Paint record starts transparent instead (see #BKE_paint_layers_channel_add): only a Fill shows
 * these defaults, so a fresh Paint covers nothing.
 */
void BKE_paint_layers_channel_default_value(const Material &ma, int channel, float r_value[4]);

/**
 * Whether \a target is reachable from \a from by following MATERIAL layers' source materials, so
 * setting \a target as a source under \a from would make the bake build a cycle.
 */
bool BKE_paint_layers_material_depends_on(const Material &from, const Material &target);

/**
 * Set the source material a #MA_PAINT_LAYER_KIND_MATERIAL \a layer bakes from, maintaining the
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
 * Change the kind of \a layer between Paint and Fill, converting what the kinds disagree on:
 * Paint keeps its channels as future maps, Fill gives them up for its constant color (a painted
 * map is not a constant), and Fill's color carries into Paint as the starting point of the first
 * stroke's map.
 *
 * Other kind changes are refused: a custom-group layer is created, not converted, and a correction
 * is born from its parent, not renamed into existence.
 *
 * \return false when \a layer is null, not part of \a ma, or \a kind is not Paint or Fill.
 */
bool BKE_paint_layers_kind_change(Material &ma,
                                  MaterialPaintLayer *layer,
                                  eMaterialPaintLayerKind kind);

/**
 * Add a correction row under \a owner: a child row of kind Correction carrying \a section and
 * \a effect. \a name may be null, in which case "Correction" is used.
 *
 * \return the new row, or null when \a owner is null or not part of \a ma, or \a section or
 * \a effect is out of range.
 */
MaterialPaintLayer *BKE_paint_layers_correction_add(Material &ma,
                                                    MaterialPaintLayer *owner,
                                                    int section,
                                                    int effect,
                                                    const char *name);

/** Set the section (#eMaterialPaintLayerCorrectionSection) of a correction row. */
bool BKE_paint_layers_correction_set_section(Material &ma,
                                             MaterialPaintLayer *correction,
                                             int section);

/** Set the effect (#eMaterialPaintLayerCorrectionEffect) of a correction row. */
bool BKE_paint_layers_correction_set_effect(Material &ma,
                                            MaterialPaintLayer *correction,
                                            int effect);

/**
 * Debug check that the split lists agree with their rows' sections: every row of
 * #MaterialPaintLayer::effects carries #MA_PAINT_LAYER_SECTION_CONTENT and every row of
 * #MaterialPaintLayer::mask_stack carries #MA_PAINT_LAYER_SECTION_MASK. The body is `BLI_assert`
 * only, so it is a no-op in a release build; the generator calls it once per build to catch a
 * storage bug early.
 */
void BKE_paint_layers_assert_consistent(const Material &ma);

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

/**
 * Start (or restart) the partial-update subscription over the maps \a layer's bake depends on.
 * Called when a bake is written, so pixel edits to those maps are noticed by
 * #BKE_paint_layers_bake_notice_changes.
 */
void BKE_paint_layers_bake_subscribe(Material &ma, MaterialPaintLayer &layer);

/** Drain \a ma's subscriptions; a pixel change marks the material stale for the planner. */
void BKE_paint_layers_bake_notice_changes(Material &ma);

/** Drop \a ma's runtime subscription state (free, copy, file load). */
void BKE_paint_layers_bake_runtime_free(Material &ma);

/**
 * The union of the pixel rectangles \a layer's source maps changed in since its last bake, as
 * \a r_region `{xmin, xmax, ymin, ymax}`, or false when nothing is pending.
 *
 * The planner uses it to re-bake only the changed tile-sized rectangle; the first bake and the
 * first check after a load are full (the subscription reports a full update).
 */
bool BKE_paint_layers_bake_changed_region(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int r_region[4]);

/**
 * Bring every baked row of \a ma current, the synchronous half of the K-1 planner.
 *
 * A row whose stored hash no longer matches is re-rendered through
 * #BKE_paint_layers_bake_render_node into its service maps, stamped and subscribed. Callers run
 * this on the main thread after draining the subscription (#BKE_paint_layers_bake_notice_changes);
 * the heavy map work is meant to move to a wmJob later. \a r_changed reports whether any row was
 * re-baked, so the caller knows to rebuild the tree.
 */
bool BKE_paint_layers_bake_ensure(Main &bmain, Material &ma, bool *r_changed = nullptr);

/**
 * The generated-node weight above which an AUTO row is baked instead of evaluated live: the number
 * of nodes the row's subtree would add to the tree. One place to tune -- the planner compares a
 * row's weight against it, so a "heavy enough to be worth caching" row is one number, not a rule.
 */
constexpr int PAINT_LAYERS_AUTO_BAKE_NODES = 24;

/**
 * Whether \a layer's bake is heavy enough to leave the main thread: its subtree would add more than
 * #PAINT_LAYERS_AUTO_BAKE_NODES nodes, or its declared map size is at least
 * #PAINT_LAYERS_HEAVY_BAKE_SIZE. A light row is baked synchronously at the K-1 point; a heavy one
 * is queued to a wmJob so the UI keeps running.
 */
bool BKE_paint_layers_bake_is_heavy(const Material &ma, const MaterialPaintLayer &layer);

/** The map side at or above which a bake counts as heavy regardless of the subtree's weight. */
constexpr int PAINT_LAYERS_HEAVY_BAKE_SIZE = 2048;

/** Whether any heavy, not-yet-valid baked row of \a ma waits for a worker. */
bool BKE_paint_layers_bake_heavy_pending(const Material &ma);

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
 * The live constant a Material row's \a channel takes from its source right now.
 *
 * The row shows its source instead of its baked map when its bake is deferred
 * (#BKE_paint_layers_bake_row_is_deferred) -- so the user sees a slider move without a bake --
 * or when this channel has no baked map yet. The second case keeps the row's channel set from
 * depending on focus: without it, a freshly added Material row would drop out of the viewport
 * the moment the user moved off it, and the root topology would change on every focus move. A
 * channel with no map is better shown as the source's constant than not shown at all.
 *
 * Only channels the resolver calls #ChannelResolution::Constant are answered here; an Image or
 * Baked channel returns false and stays on its baked map. A row that is not deferred and already
 * has a map for the channel wins with the map.
 *
 * The generator and the CPU compositor both ask this, so the two cannot disagree about
 * what the row is showing.
 *
 * \param r_value: the source's socket default -- x for a scalar channel, xyz for a colour
 *                 one, matching #MaterialSourceResolve.constants.
 */
bool BKE_paint_layers_material_live_constant(const Material &ma,
                                             const MaterialPaintLayer &layer,
                                             int channel,
                                             float r_value[4],
                                             const PaintLayersRegenCache *cache = nullptr);

/**
 * The live map an active Material row's \a channel takes from its source right now.
 *
 * Mirrors #BKE_paint_layers_material_live_constant for #ChannelResolution::Image, under
 * the same rule: the row is deferred, or this channel has no baked map yet.
 *
 * Only a trivially mapped texture qualifies -- flat projection and nothing linked to the
 * node's Vector input. The CPU compositor samples an #ImBuf straight in UV space and
 * reproduces neither a mapping chain nor a projection, so anything else would make the two
 * sides disagree and the picture jump when the row is left.
 */
bool BKE_paint_layers_material_live_image(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int channel,
                                          Image **r_image,
                                          const ImageUser **r_iuser,
                                          const PaintLayersRegenCache *cache = nullptr);

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
 * What one #BKE_paint_layers_regenerate call learns once and reuses: it lives on that call's stack
 * and is handed down, never kept between calls, so nothing outside a regeneration can read a stale
 * answer. Every reader takes it as an optional pointer; without one the answer is recomputed, which
 * is what bake, the CPU composite, RNA and the editors do.
 */
struct PaintLayersRegenCache {
  /**
   * Whether #modes may be filled. The sampler-budget fallback moves rows' modes (a forced bake), so
   * a mode cached before it is done would outlive the change; the regeneration raises this once the
   * final forced set is known.
   */
  bool modes_frozen = false;
  /** Rows' modes, filled lazily and only while #modes_frozen. */
  mutable Map<const MaterialPaintLayer *, PaintLayerMaterialMode> modes;
  /** #BKE_paint_layers_material_bake_ready per row: hashing the source tree is not free. */
  mutable Map<const MaterialPaintLayer *, bool> bake_ready;
  /** The Pass Through visibility multiplier of every row, from one walk of the stack. */
  mutable Map<const MaterialPaintLayer *, float> pass_through_scales;
  mutable bool pass_through_scales_valid = false;

  /**
   * The resolve of \a source, computed on first ask. A source's node tree is not written while a
   * stack is regenerated (only the owner's is), so the answer holds for the whole call. The entry is
   * heap-allocated, so the returned reference stays valid when later ones are added.
   */
  const MaterialSourceResolve &resolve(const Material *source) const;

  /**
   * #resolve through \a cache when there is one, else a fresh resolve held in \a r_local. Either way
   * the caller reads the returned reference and never copies the resolve.
   */
  static const MaterialSourceResolve &resolve_get(const Material *source,
                                                  const PaintLayersRegenCache *cache,
                                                  MaterialSourceResolve &r_local);

  /**
   * Drop what depends on the owner's own node tree, for a caller about to rewrite it: a row that
   * reads its owner as its source would otherwise keep the answer from before the rewrite.
   */
  void invalidate_for_owner(const Material &owner);

 private:
  mutable Map<const Material *, std::unique_ptr<MaterialSourceResolve>> resolves_;
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
 * Whether a sampler-budget fallback pinned \a layer onto its baked maps for this session. Runtime
 * state only: it is recomputed from the budget on every regeneration, lifted as soon as the budget
 * allows, and never saved. #BKE_paint_layers_material_mode reports #PaintLayerMaterialMode::Baked
 * while this holds, which is what makes the topology hash see the mode change and rebuild once.
 */
bool BKE_paint_layers_material_forced_bake(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether the bake of a Material row can be shown: its stored hash matches the source and none of
 * its maps is being rendered right now. The hash alone is not enough, because the hand-over stamps
 * it before the worker has written any pixel. A row that is not ready stays live, so a stale, blank
 * or half-written map is never shown; it moves to its maps once, when the last map lands.
 */
bool BKE_paint_layers_material_bake_ready(const Material &ma, const MaterialPaintLayer &layer);

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

/** Render every collected row from the localized copy. Runs on the worker thread. */
void BKE_paint_layers_bake_job_compute(PaintLayersBakeJob &job);

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

/**
 * The single substitution decision the generator and the CPU compositor share: whether \a layer's
 * baked map for \a channel may stand in for its live subtree right now, and the map.
 *
 * \return true and sets \a r_image when #BKE_paint_layers_bake_is_valid holds and a baked map exists
 * for the channel. Never reads a baked map any other way, so the two sides cannot disagree.
 */
bool BKE_paint_layers_bake_substitute(const Material &ma,
                                      const MaterialPaintLayer &layer,
                                      int channel,
                                      Image **r_image);

/**
 * The C-7 fallback for a Custom layer when its bake is not current.
 *
 * A Custom group has no CPU expression at all, so a render without a GPU context -- or one that
 * started before the first bake landed -- has nothing live to fall back on. When maps exist from an
 * earlier bake they are still shown, flagged stale through \a r_stale, rather than dropping the row
 * to black; when no maps exist the row is skipped (the result below passes through). Only Custom is
 * covered: every other kind has a live subtree the strict #BKE_paint_layers_bake_substitute leaves in
 * place.
 *
 * \return true and sets \a r_image when a color map and a coverage map exist for \a channel.
 */
bool BKE_paint_layers_bake_substitute_custom(const Material &ma,
                                             const MaterialPaintLayer &layer,
                                             int channel,
                                             Image **r_image,
                                             bool *r_stale);

/**
 * Composite \a layer's subtree for \a channel the way a bake of it would: the isolated-group model
 * (`P/a`, the same the CPU already computes for a folder), producing the node's straight scene-
 * linear colour and its grey coverage.
 *
 * \a r_color_rgba is `size * size * 4` floats, \a r_coverage_gray `size * size`. The node's own maps
 * are composited at their resolution and nearest-sampled to \a size. The bake planner and the
 * generator's substitution read this one function's result, so a baked map means one thing.
 *
 * \param dst_rect: when given, only this `{x0, y0, x1, y1}` rectangle of the `size`-square result
 *                  is computed and written; the caller passes it for a partial re-bake so the
 *                  compute follows the changed tile instead of the whole map. The rest of the
 *                  outputs is left untouched. The rectangle is clamped to `size`.
 *
 * \return false when \a layer is not in \a ma, does not take part in \a channel, or the composite
 * fails.
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
 * Render \a row's result in \a channel -- the same content a bake of it stores, through
 * #BKE_paint_layers_bake_render_node -- into \a dst, which must already be \a size square and carry
 * the channel's colorspace. Used by the "Use Row Result" operator to copy one row's content into
 * another row's channel map.
 */
bool BKE_paint_layers_bake_row_to_image(const Material &ma,
                                        const MaterialPaintLayer &row,
                                        int channel,
                                        int size,
                                        Image &dst);

/**
 * Whether a value edit or a pixel change left some row of \a ma waiting to be re-baked.
 *
 * A scheduler signal only ("something in this material changed"), never the per-row answer: which
 * row is stale is #BKE_paint_layers_bake_is_valid's question.
 */
bool BKE_paint_layers_bake_stale_get(const Material &ma);
/** Clear \a ma's pending-bake mark; the planner calls this once the queue is drained. */
void BKE_paint_layers_bake_stale_clear(Material &ma);

/** Whether \a ma has a Material row left behind by the active row moving. */
bool BKE_paint_layers_material_bake_due_get(const Material &ma);
/** Clear \a ma's due mark; the editor planner calls this once it has run. */
void BKE_paint_layers_material_bake_due_clear(Material &ma);

/**
 * Allocate \a layer's bake cache if it has none and return it, without marking anything: the caller
 * owns the edit (mode, size, a baked map) and tags the description itself.
 */
MaterialPaintLayerBake *BKE_paint_layers_bake_ensure(MaterialPaintLayer &layer);

/** Drop \a layer's bake cache and mark the generated tree stale. */
bool BKE_paint_layers_bake_clear(Material &ma, MaterialPaintLayer &layer);

/**
 * Point \a layer's baked map for \a channel at \a image (negative \a channel is its coverage map),
 * maintaining the image's user count. The caller -- a material bake handing over its result, or the
 * synchronous planner -- owns \a image. Marks the tree stale; the bake hash is only settled by
 * #BKE_paint_layers_bake_finalize once every map of the row is in place.
 */
bool BKE_paint_layers_bake_set_map(Material &ma,
                                   MaterialPaintLayer &layer,
                                   int channel,
                                   Image *image);

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
 */
void BKE_paint_layers_material_bake_apply(Main &bmain,
                                          Material &ma,
                                          MaterialPaintLayer &layer,
                                          int size,
                                          Span<int> channels,
                                          Span<Image *> images);

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
 * Structure and parameters only -- kind, blend, flags, opacity, fill colour, channel/mask maps by
 * `session_uid`, mask value, effects, mask items, children order, the source material, a Custom
 * group, its IDProperty inputs, and the bake size. Pixel edits are not visible here; they arrive
 * through the partial-update subscription and settle as a re-bake of the changed rectangle.
 */
void BKE_paint_layers_bake_hash(const MaterialPaintLayer &layer, uint32_t r_hash[2]);

/**
 * Whether \a layer's stored bake can stand in for its live subtree right now.
 *
 * True when a bake exists, its stored hash matches #BKE_paint_layers_bake_hash, and the runtime
 * partial-update subscription has reported no change to the source maps since the bake. The
 * subscription is runtime-only: a freshly loaded file has none, so the first check re-bakes.
 */
bool BKE_paint_layers_bake_is_valid(const Material &ma, const MaterialPaintLayer &layer);

/** Set whether \a layer takes part in the stack. */
bool BKE_paint_layers_set_enabled(Material &ma, MaterialPaintLayer *layer, bool enabled);

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

}  // namespace blender
