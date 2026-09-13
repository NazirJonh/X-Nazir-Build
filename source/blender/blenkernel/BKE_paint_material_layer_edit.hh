/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Editing a material's paint layer stack by rewriting its node graph.
 *
 * The stack has no storage of its own -- it *is* the chain of Mix nodes that
 * #BKE_paint_material_layer_stack_from_material reads back -- so moving a layer means relinking
 * nodes, in every channel at once, or not at all. Everything here is therefore transactional: the
 * preconditions are checked across all channels before the first link is touched, and a stack that
 * fails them is left exactly as it was with a reason the UI can show.
 *
 * \note One layer of the stack is not a Mix node: the bottom one is a bare Image Texture, since it
 * has nothing underneath to blend with. Operations that would change *which* layer is the bottom
 * are refused here rather than half-implemented -- see
 * #PaintMaterialLayerEditError::IsBottomLayer.
 */

#include <cstdint>

#include "BKE_paint_material_layer_model.hh"

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"
#include "BLI_uuid.h"

namespace blender {

struct Image;
struct Main;
struct Material;
struct bNode;
struct bNodeSocket;
struct bNodeTree;

/**
 * Runtime-only state a material's paint layer stack carries next to its graph.
 *
 * The edit paths below bump #edit_revision on every successful edit, so a reader that has already
 * walked the stack can tell it moved on without walking it again. Never part of the file: the
 * reader nulls the owning pointer, a copy starts fresh, and the first edit allocates it.
 */
struct MaterialPaintLayerRuntime {
  uint64_t edit_revision = 0;
};

/**
 * The material's paint layer runtime state, allocating it on first use.
 *
 * Only an edit path has a reason to allocate; readers take #BKE_material_paint_layer_revision_get,
 * which answers for a material that never edited its stack without allocating anything.
 */
MaterialPaintLayerRuntime &BKE_material_paint_layer_runtime_get(Material &ma);
/**
 * The material's paint layer edit revision, or 0 when its runtime state was never touched.
 */
uint64_t BKE_material_paint_layer_revision_get(const Material &ma);

/**
 * The persistent identity of a paint layer -- "marker C" -- as an id-property on every Mix node
 * that layer owns, in every channel.
 *
 * A layer is N parallel Mix chains, one per channel, and reordering has to move all of them
 * together. Node identifiers cannot say which node in the Roughness chain is the same layer as one
 * in the Base Color chain: they are unique per node tree and re-issued whenever a node is copied
 * between trees. A shared UUID can, and survives group/ungroup and duplication.
 *
 * Stored as #IDP_STRING because #IDProperty has no UUID type; the format is the one
 * #BLI_uuid_format writes.
 */
bUUID BKE_paint_material_layer_marker_get(const bNode &node);
void BKE_paint_material_layer_marker_set(bNode &node, const bUUID &layer_id);

/**
 * Get/set the color tag of a layer group node.
 *
 * Color tags are stored as an IDProperty on the group node, separate from the node's own color.
 * -1 means no color tag is set. Valid range is 0-7, matching Grease Pencil layer groups.
 */
int BKE_paint_material_layer_color_tag_get(const bNode &node);
void BKE_paint_material_layer_color_tag_set(bNode &node, int color_tag);

PaintMaterialLayerKind BKE_paint_material_layer_kind_get(const bNode &node);
void BKE_paint_material_layer_kind_set(bNode &node, PaintMaterialLayerKind kind);

/**
 * The colour a #PaintMaterialLayerKind::Fill layer stands for.
 *
 * The marker rather than the pixels is the source of truth: a fill map can be painted over, and
 * a painted-over map cannot be asked what colour it was filled with. Returns false, leaving
 * \a r_color untouched, for a layer that carries no fill colour.
 */
bool BKE_paint_material_layer_fill_color_get(const bNode &node, float r_color[4]);
void BKE_paint_material_layer_fill_color_set(bNode &node, const float color[4]);

/** Why an edit was refused. Never partially applied. */
enum class PaintMaterialLayerEditError : int8_t {
  None = 0,
  /** The material or its node tree is linked or overridden. */
  NotEditable,
  /** No channel of this material resolves to a layer stack. */
  NotAStack,
  /** The ordinal names no layer. */
  IndexOutOfRange,
  /**
   * The bottom layer is a bare Image Texture rather than a Mix node, so it cannot change places
   * with a layer that blends.
   */
  IsBottomLayer,
  /** A Mix node's result is consumed by something besides the layer above it. */
  ChainIsShared,
  /** A reroute or an unrecognized node sits in the chain. */
  ChainNotPlain,
  /** The channels do not agree on how many layers there are. */
  ChannelsDisagree,
  /** A stack has to start at a Principled BSDF, and this material has none. */
  NoPrincipled,
  /** A map or one of the nodes the layer needs could not be created. */
  CreationFailed,
  /**
   * The ordinal names a layer held inside a group.
   *
   * Editing those means rewriting the group's own node tree, which these functions do not do yet;
   * ungroup first, or act on the group row instead.
   */
  HasGroups,
  /** The move would nest groups deeper than the readers walk. */
  NestingTooDeep,
  /** A layer group's node tree is linked, so nothing in it may be written. */
  TreeNotEditable,
  /** A layer group's node tree is a library override, so nothing in it may be written. */
  TreeIsOverride,
  /**
   * A layer group's node tree is reached from another material's node tree.
   *
   * Editing the group would silently edit a stack this call was never shown; making a private
   * copy of the group first is the way out, and nothing here does that implicitly.
   */
  TreeShared,
  /**
   * A channel needed for the operation is wired to a node network that is not a paint
   * layer stack (procedural, group output, foreign mask). Rewiring it would silently
   * destroy the material's look, so the operation refuses before baking anything.
   */
  ChannelHasUnsupportedSource,
  /** The row is a folder: its channels are what the layers inside it make them. */
  ChannelNotToggleable,
  /**
   * A layer is found through the maps its enabled channels bind as paint targets; switching off
   * the last one would lose the active layer. Hiding the layer is the way to take it out.
   */
  LastEnabledChannel,
  /** No correction anywhere in the stack carries the marker the operation named. */
  CorrectionNotFound,
  /**
   * The operation does not apply to the correction's section: a mask correction has no
   * per-channel switch, its form in each channel follows the row (spec 18 §4.3).
   */
  CorrectionSectionMismatch,
  /**
   * A content correction hangs on the layer's content stack, which a group keeps inside its own
   * folder -- there is nothing on the group row itself to correct. A mask section limits the
   * folder's result and is allowed.
   */
  CorrectionNotAllowedOnGroup,
  /** A correction's nodes are not the shape this module builds (spec 18 §4.1a). */
  CorrectionChainNotPlain,
  /**
   * The folder carries mask corrections, which shape the folder's result as a whole; once the
   * rows are spliced out there is no result left for them to shape.
   */
  GroupHasMaskCorrections,
};

/** A message for #BKE_report, already translated at the call site by the caller if needed. */
const char *BKE_paint_material_layer_edit_error_message(PaintMaterialLayerEditError error);

/**
 * Give every layer of \a ma a marker, matching layers across channels first by an existing marker,
 * then by #Image.paint_layer_id, then by position.
 *
 * Called before the first edit of a stack that was authored elsewhere. Modifies the graph, so it
 * belongs inside the caller's undo step.
 *
 * \return false when the stack cannot be read at all; markers are then untouched.
 */
bool BKE_paint_material_layer_markers_ensure(Material &ma);

/** A map a caller already has for one channel of a layer about to be added. */
struct PaintMaterialLayerChannelImage {
  /** An #eMaterialPaintChannel. */
  int channel = -1;
  Image *image = nullptr;
};

/** How a layer is created. Defaults describe "an empty layer on top of the stack". */
struct PaintMaterialLayerAddParams {
  /**
   * What the new layer is. The add stamps the kind marker itself; a #Paint layer carries no
   * marker, which reads the same.
   */
  PaintMaterialLayerKind kind = PaintMaterialLayerKind::Paint;
  /**
   * Where the new layer ends up. -1 puts it on top; 0 is refused, since the bottom layer is a bare
   * Image Texture rather than a Mix node (#PaintMaterialLayerEditError::IsBottomLayer).
   *
   * Ignored when #anchor_ordinal is set.
   */
  int ordinal = -1;
  /**
   * Place the new layer relative to the UI row this names, instead of at #ordinal.
   *
   * A row inside a folder inserts into that folder, directly above the named row; a row that is
   * itself a folder inserts into it, on top of what it holds (an empty folder gets its first
   * layer this way). A plain top-level row inserts directly above itself. -1 falls back to
   * #ordinal.
   */
  int anchor_ordinal = -1;
  int image_size = 1024;
  /** Used when #kind is Fill; non-color channels take the red component. */
  float fill_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  /** Name for the layer; null uses the channel-scoped default the paint code already uses. */
  const char *name = nullptr;
  /**
   * Maps the caller already has, used as the new layer's map in their channels instead of creating
   * one. A channel not listed is Absent in the new row. With no maps given at all, the row is an
   * empty Paint or Fill layer and gets a fresh map in Base Color only. On a material with no stack
   * yet, only a Base Color map is used: the first row is built in Base Color alone.
   *
   * Ownership passes to #BKE_paint_material_layer_add with the call, whatever it returns: an image
   * a new node shows keeps the user it was created with as that node's user, and every other one
   * -- all of them when the add is refused, and any for a channel the layer is not created in --
   * is
   * freed. Hand over fresh, otherwise unreferenced data-blocks only, and do not touch them after
   * the call except through the layer.
   */
  Span<PaintMaterialLayerChannelImage> channel_images;
};

/**
 * Add a paint layer to \a ma, in every channel that is already wired as a stack.
 *
 * The new row is stamped with #PaintMaterialLayerAddParams::kind on its own nodes; a #Paint
 * layer carries no marker, which reads the same.
 *
 * A material with no stack at all gets one: a single Image Texture on Base Color, which is the
 * bottom layer. The other channels are left to #BKE_paint_material_images_ensure_writable, which
 * creates a channel's map the first time a brush actually writes to it.
 *
 * Transactional like the rest of this file: every node is created and validated before the first
 * link is rewritten, so a channel that cannot take the layer leaves the graph untouched.
 *
 * With #PaintMaterialLayerAddParams::anchor_ordinal set, the layer is created inside whichever
 * folder the anchor row lives in (or the folder the anchor row is), writing to that group's own
 * node tree.
 *
 * \param r_ordinal: when given, receives the position the new layer ended up at -- a
 * #PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE-based ordinal when the layer landed inside a folder.
 */
bool BKE_paint_material_layer_add(Main &bmain,
                                  Material &ma,
                                  const PaintMaterialLayerAddParams &params,
                                  int *r_ordinal = nullptr,
                                  PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Move the layer at \a from_ordinal so that it ends up at \a to_ordinal, in every channel.
 *
 * Only the "what is below me" links are rewritten; every layer keeps its own maps, factor, blend
 * mode and node identifier.
 */
bool BKE_paint_material_layer_reorder(Main &bmain,
                                      Material &ma,
                                      int from_ordinal,
                                      int to_ordinal,
                                      PaintMaterialLayerEditError *r_error = nullptr);

/** Where a moved layer lands relative to the row it was aimed at. */
enum class PaintMaterialLayerMovePlace : int8_t {
  /** Directly above the anchor, in the anchor's own chain. */
  Above = 0,
  /** Directly below the anchor, in the anchor's own chain. */
  Below,
  /**
   * Inside the anchor, which has to be a group: on top of what it holds.
   *
   * A place of its own rather than "above the group's topmost row", because an empty group has no
   * row to name -- and an empty group is exactly what a folder is when it is made to be filled.
   */
  Into,
};

/**
 * Give every chain of \a ma the same shape: a stack of Mix nodes all the way down.
 *
 * A chain written before this contract -- or wired by hand in the Shader Editor -- can end in a
 * bare Image Texture, which is a layer with no blend mode, no opacity and nothing under it, so
 * nothing can be put below it either. This wraps such a bottom in a Mix node that blends the image
 * over the transparency its own unlinked socket holds, exactly like every layer above it.
 *
 * The render changes only where that bottom map is not opaque: it used to hand its colour to the
 * channel whatever its alpha said, and now it covers what is below it the way a layer does.
 *
 * The edit operations here call this only after their preflight passed, so a refused edit does not
 * convert anything; they hand out layer identities themselves, in their own step, and no longer
 * get them from this function as a side effect. A caller that needs both does what they do:
 * convert first, then call #BKE_paint_material_layer_markers_ensure.
 *
 * \return whether anything was converted.
 */
bool BKE_paint_material_layer_bottom_normalize(Main &bmain, Material &ma);

/**
 * Move the layer at \a from_ordinal next to the layer at \a anchor_ordinal, in every channel.
 *
 * Where #BKE_paint_material_layer_reorder names the destination by the position the layer should
 * end up at, this names it by a row to land beside: \a above puts the layer directly above the
 * anchor, otherwise directly below it. That is what a drop can express and a position cannot --
 * "above the topmost layer of that group" is a place no existing row is numbered by -- and it is
 * also what frees the caller from having to undo the shift the layer's own removal causes.
 *
 * The anchor's chain is the destination, so an anchor inside a group moves the layer into that
 * group, and one outside moves it out. Moving between two trees copies the layer's nodes into the
 * destination tree and removes the originals; the layer keeps its maps, mask, factor and blend
 * mode either way.
 *
 * Fails with #PaintMaterialLayerEditError::IsBottomLayer when the moved layer or the destination
 * position is the bottom of a chain, which is a bare Image Texture rather than a blended layer.
 *
 * \param r_ordinal: when given, receives the ordinal the moved row has after the move -- which
 * a caller needs to keep it selected, since a move renumbers rows past it.
 */
bool BKE_paint_material_layer_move(Main &bmain,
                                   Material &ma,
                                   int from_ordinal,
                                   int anchor_ordinal,
                                   PaintMaterialLayerMovePlace place,
                                   int *r_ordinal = nullptr,
                                   PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Give the layer at \a ordinal a mask: one image driving the Factor of its Mix nodes in every
 * channel.
 *
 * \note A layer without a mask takes its Factor from its own map's Alpha, so adding a mask
 * *replaces* what covers the layer rather than multiplying with it. That is a consequence of the
 * node contract -- a layer's Factor is either an image or nothing, never an expression -- so the
 * new mask starts out white by default and the layer therefore covers everything below it until
 * the
 * mask is painted. Callers can instead ask for a black fill to start with the layer hidden.
 *
 * The fill is a color rather than a black-or-white flag: what the mask starts as is a property of
 * the generated image, and a third kind of fill is a caller's color, not a new parameter here.
 *
 * Fails with #PaintMaterialLayerEditError::IsBottomLayer for the bottom layer, which has no Mix
 * node to mask, and does nothing when the layer already has one. Groups are layers too: their mask
 * controls the coverage of the group's result.
 */
bool BKE_paint_material_layer_mask_add(Main &bmain,
                                       Material &ma,
                                       int ordinal,
                                       const float initial_color[4],
                                       int image_size = 1024,
                                       PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Remove the layer's mask, putting its own map's Alpha back on the Factor.
 *
 * The mask #Image is left to the usual user-count rules; only the nodes that read it go.
 */
bool BKE_paint_material_layer_mask_remove(Main &bmain,
                                          Material &ma,
                                          int ordinal,
                                          PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Whether \a image is the mask of any layer of \a ma's stack, groups included. A pure reader for
 * editor code that has to know if the mask being painted still belongs to a stack -- after a
 * focus switch, before trusting it as a stroke target -- without caring which row owns it.
 */
bool BKE_paint_material_layer_stack_contains_mask(Main &bmain, Material &ma, const Image &image);

/**
 * Add a correction on the layer at \a layer_ordinal, in every channel that is already wired as a
 * stack (spec 18 §4.1).
 *
 * The correction starts Absent in every channel: no map, its coverage unlinked and zero -- it
 * contributes nothing until a map is set for it. A #PaintMaterialCorrectionSection::Mask
 * correction is the exception: a mask has no per-channel choice (spec 18 §4.3), so it gets one
 * transparent map right away, shared by every channel and sized by
 * #BKE_paint_material_layer_map_size_get.
 *
 * A content correction hangs on the layer's own content stack and is refused on a group row
 * (#PaintMaterialLayerEditError::CorrectionNotAllowedOnGroup), since a group's content stays
 * inside its folder; a mask section limits the folder's result and is allowed. A bare base is
 * supported: the shape conversion runs first, and the correction hangs on the Mix the base
 * becomes.
 *
 * Transactional like every edit here: the preconditions are checked across all channels before
 * the first node is created.
 *
 * \param name: the row's display name, stored as the label of the correction's Mix nodes; null
 *   leaves the row unnamed, which the stack model reads as "Correction".
 * \param r_correction: when given, receives the new correction's identity -- the marker its nodes
 *   carry in every channel and its map is tagged with.
 */
bool BKE_paint_material_layer_correction_add(Main &bmain,
                                            Material &ma,
                                            int layer_ordinal,
                                            PaintMaterialCorrectionSection section,
                                            const char *name,
                                            bUUID *r_correction = nullptr,
                                            PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Remove the correction \a correction from every channel, closing the chain over it.
 *
 * The layer reads as if the correction had never been there: what the correction's nodes blended
 * over feeds whatever consumed their result, in every channel at once. The correction's map node
 * goes with it when nothing else reads it; the #Image data-block itself is left to the usual
 * user-count rules, since a map the user painted is not this function's to delete.
 */
bool BKE_paint_material_layer_correction_remove(Main &bmain,
                                                Material &ma,
                                                const bUUID &correction,
                                                PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Move the correction \a correction so that it ends up at \a new_index of its section, in every
 * channel.
 *
 * Only the links between the section's corrections are rewritten; the nodes keep their maps,
 * opacity and identity, and no node is created or removed. \a new_index counts from the bottom of
 * the section, the way the stack model lists its correction rows.
 */
bool BKE_paint_material_layer_correction_reorder(Main &bmain,
                                                Material &ma,
                                                const bUUID &correction,
                                                int new_index,
                                                PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Turn the correction \a correction on or off, in every channel.
 *
 * Implemented by muting the correction's nodes, which is what the stack model reads back as
 * "disabled" -- the same graph mutation toggling a layer is, and it changes what renders exactly
 * as toggling the nodes by hand would.
 */
bool BKE_paint_material_layer_correction_set_enabled(Main &bmain,
                                                    Material &ma,
                                                    const bUUID &correction,
                                                    bool enable,
                                                    PaintMaterialLayerEditError *r_error =
                                                        nullptr);

/**
 * Set the correction \a correction's opacity (0..1) in every channel at once.
 *
 * Unlike a layer's own Opacity -- one node per channel, genuinely independent -- a correction's
 * opacity is meant to read as one shared strength regardless of which channel's Blending Mode the
 * stack currently shows (plan 19 review): painting the correction's map, or picking its per-channel
 * Blending Mode, still varies by channel, but how hard the correction pushes does not. Implemented
 * the same way #BKE_paint_material_layer_correction_set_enabled keeps Enabled shared: every
 * channel's own coverage-multiply constant is written to the same value.
 */
bool BKE_paint_material_layer_correction_opacity_set(Main &bmain,
                                                     Material &ma,
                                                     const bUUID &correction,
                                                     float opacity,
                                                     PaintMaterialLayerEditError *r_error =
                                                         nullptr);

/**
 * The state of \a channel on the correction \a correction, read from the graph alone -- the
 * per-channel half of #BKE_paint_material_layer_correction_set_enabled, the way
 * #BKE_paint_material_layer_channel_state_get is the one of a layer row's toggle.
 *
 * A channel with no graph of its own (AO) reads by its tagged map alone: the map exists or it
 * does not, and there is no Disabled form to keep it in. Every other channel reads the
 * correction's nodes: no chain, no correction in the chain, or a shape this module does not
 * build all read as Absent.
 */
PaintMaterialLayerChannelState BKE_paint_material_layer_correction_channel_state_get(
    Main &bmain, Material &ma, const bUUID &correction, int channel);

/**
 * Switch \a channel of the correction \a correction on or off.
 *
 * Off keeps the map (Disabled), the way a layer row's toggle does. On restores a Disabled map,
 * or gives an Absent channel a fresh one sized by #BKE_paint_material_layer_map_size_get. A
 * channel the material has not wired yet gets its chain first, corrections included; a channel
 * with no graph of its own (AO) keeps only the tagged map -- there are no nodes to build, and
 * switching it off untags rather than mutes, since the map is all the state there is.
 *
 * When the row's base is Absent or Disabled in this channel, the coverage its content
 * corrections accumulate is what still paints (spec 18 I2'): enabling a channel relinks it into
 * the row's coverage input wherever nothing owns that input, and disabling takes the link back
 * out once no correction contributes there anymore. A mask owns the coverage input and is never
 * touched.
 */
bool BKE_paint_material_layer_correction_channel_enabled_set(Main &bmain,
                                                            Material &ma,
                                                            const bUUID &correction,
                                                            int channel,
                                                            bool enable,
                                                            PaintMaterialLayerEditError *r_error =
                                                                nullptr);

/**
 * Rename the correction \a correction.
 *
 * The name a user sees is the label of the correction's Mix nodes, so this sets it in every
 * channel at once.
 */
bool BKE_paint_material_layer_correction_rename(Main &bmain,
                                                Material &ma,
                                                const bUUID &correction,
                                                StringRef name,
                                                PaintMaterialLayerEditError *r_error = nullptr);

/**
 * The ordinal of the layer holding \a correction, or -1 when no correction anywhere in the stack
 * carries it.
 *
 * \a r_section and \a r_index are optional: the section the correction hangs on, and its position
 * in it, counting from the bottom.
 */
int BKE_paint_material_layer_correction_owner_ordinal(
    Main &bmain,
    Material &ma,
    const bUUID &correction,
    PaintMaterialCorrectionSection *r_section = nullptr,
    int *r_index = nullptr);

/** One source of a correction copy: the material it hangs on, and its identity there. */
struct PaintMaterialCorrectionRef {
  /** The #ID.session_uid of the material the correction to copy lives on. */
  uint32_t material_session_uid = 0;
  /** The correction's marker in that material. */
  bUUID correction = {};
};

/** What a copy skipped or had to build along the way, for a caller that reports it. */
struct PaintMaterialCorrectionCopyReport {
  /** Refs whose material or correction was not found; the rest of the call carried on without
   * them. */
  int skipped_missing = 0;
  /** Content corrections aimed at a group row, which keeps its content inside its folder. */
  int skipped_group = 0;
  /** The parent channels the copy wired into the target because a copied channel needed them. */
  Vector<int> enabled_parent_channels;
  /** How many copied maps were scaled to the size the target's own maps have. */
  int scaled_maps = 0;
};

/**
 * Copy the corrections \a sources name onto the layer at \a target_layer_ordinal of \a target,
 * maps and all -- across materials as readily as within one (spec 18 §4.5).
 *
 * Every copy is a new row with a new identity and new #Image data-blocks, the way a duplicate is.
 * The source's name, blend, opacity, on/off state and per-channel channels come along, and a
 * channel the source has the correction on gets wired into the target first: copying a Metallic
 * correction is what gives a target without a Metallic chain one. A copied map is scaled to the
 * size the target's own maps have.
 *
 * A ref whose material or correction cannot be found is skipped and counted in the report; so is
 * a content correction aimed at a group row, since the group's content lives inside its folder.
 * Any other refusal fails the call -- before the first write when the preflight gives it, as a
 * partial result for the caller's undo step to take back when it happens mid-copy.
 *
 * \param r_created: receives the new corrections' markers, one per copied ref, in order.
 */
bool BKE_paint_material_layer_corrections_copy(Main &bmain,
                                              Span<PaintMaterialCorrectionRef> sources,
                                              Material &target,
                                              int target_layer_ordinal,
                                              Vector<bUUID> &r_created,
                                              PaintMaterialCorrectionCopyReport *r_report =
                                                  nullptr,
                                              PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Scale every correction map of the layer at \a layer_ordinal to \a width x \a height (spec 18
 * D8): a correction's maps are its pixels, so a layer moved to a new resolution takes them along,
 * whichever channel or section they paint and whether their channel is on or off.
 *
 * False when the ordinal names no layer, leaving every map untouched.
 */
bool BKE_paint_material_layer_corrections_scale(Main &bmain,
                                                Material &ma,
                                                int layer_ordinal,
                                                int width,
                                                int height);

/**
 * The map size a new map of the layer at \a layer_ordinal should have (spec 18 §6.1): the first
 * existing map of the layer -- any channel, a mask included -- then the maps of its corrections,
 * then the size the stack composites at, read from the Base Color channel.
 *
 * False when the stack gives up no size at all, leaving \a r_width and \a r_height untouched.
 */
bool BKE_paint_material_layer_map_size_get(Main &bmain,
                                           Material &ma,
                                           int layer_ordinal,
                                           int &r_width,
                                           int &r_height);

/**
 * Set \a image as the map of \a channel on the layer at \a ordinal, leaving every other channel
 * of the layer exactly as it was.
 *
 * This is what a dropped image means: the layer at the drop shows \a image through \a channel. A
 * bare base is supported and is the simplest case -- the base is its channel's own map, so this
 * replaces that Image Texture's image. A map the layer already shows in \a channel is reused
 * when it is this layer's own Image Texture node, and replaced otherwise.
 *
 * Transactional like the rest of this file. The image is referenced by the map node's ID field,
 * and the node tree update recounts node ID users, so a caller that received \a image from a
 * load helper hands the load's extra user back with #id_us_min before calling -- the way a drop
 * does.
 *
 * \param channel: an #eMaterialPaintChannel the material has wired as a stack; an unwired one is
 *   refused (#PaintMaterialLayerEditError::IndexOutOfRange).
 */
bool BKE_paint_material_layer_channel_image_set(Main &bmain,
                                                Material &ma,
                                                int ordinal,
                                                int channel,
                                                Image &image,
                                                PaintMaterialLayerEditError *r_error = nullptr);

/**
 * A fresh transparent map for \a channel, \a size pixels square: a layer that shows it in
 * \a channel contributes nothing there, since its coverage comes from the map's alpha.
 *
 * This is how a layer drops one channel without breaking the stack's shape, which needs every
 * channel's chain to hold the same rows. The image comes with the one user a new data-block has;
 * see #BKE_paint_material_layer_channel_image_set for handing it over.
 */
Image *BKE_paint_material_layer_neutral_image_create(Main &bmain, int channel, int size);

/**
 * Re-fill the maps of the Fill layer at \a ordinal with \a color, in every wired channel, and
 * record the colour on the layer's marker.
 *
 * Refused with #PaintMaterialLayerEditError::IndexOutOfRange for a layer that is not
 * #PaintMaterialLayerKind::Fill: re-filling a painted layer would silently destroy work.
 */
bool BKE_paint_material_layer_fill_color_apply(Main &bmain,
                                               Material &ma,
                                               int ordinal,
                                               const float color[4],
                                               PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Live preview for the Fill color picker: re-fill every wired channel's map with \a color,
 * exactly like #BKE_paint_material_layer_fill_color_apply, but record nothing on the layer's
 * marker and push no undo step.
 *
 * The picker's RNA update calls this per tick; the dialog's exec does the real apply (pixels +
 * marker + undo) once, cancel restores the start color through this same path.
 *
 * Refused with #PaintMaterialLayerEditError::IndexOutOfRange for a layer that is not
 * #PaintMaterialLayerKind::Fill, writing nothing.
 */
bool BKE_paint_material_layer_fill_color_preview(Main &bmain,
                                                 Material &ma,
                                                 int ordinal,
                                                 const float color[4],
                                                 PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Record \a kind on the layer at \a ordinal, on its node in every channel at once.
 *
 * This is the marker half of a layer's kind; the pixels are the caller's business (a Fill re-fills
 * its maps, a Material's maps are the baked ones). A bare base is accepted: the marker is what
 * makes a bare Image Texture read as anything other than a Paint layer.
 */
bool BKE_paint_material_layer_kind_set(Main &bmain,
                                       Material &ma,
                                       int ordinal,
                                       PaintMaterialLayerKind kind,
                                       PaintMaterialLayerEditError *r_error = nullptr);

/**
 * The channels of \a ma that resolve to a layer stack, in #eMaterialPaintChannel order.
 *
 * What "every channel that is already wired as a stack" enumerates to. A UI offering a choice of
 * channels for a layer edit asks here rather than assuming the material's full channel set -- a
 * material may wire Base Color and leave Roughness constant.
 */
void BKE_paint_material_layer_channels_wired(Material &ma, Vector<int> &r_channels);

/**
 * Copy the layer group or layer at \a ordinal, maps and all, and put the copy directly above it.
 *
 * The copies are new #Image data-blocks with the same pixels: a duplicated layer that shared its
 * maps would not be a copy, it would be the same layer listed twice. A group copy gets a private
 * node tree with new markers for its contents.
 *
 * \param r_ordinal: when given, receives the position of the copy.
 */
bool BKE_paint_material_layer_duplicate(Main &bmain,
                                        Material &ma,
                                        int ordinal,
                                        int *r_ordinal = nullptr,
                                        PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Rename the layer at \a ordinal.
 *
 * The name a user sees is the label of the layer's Mix nodes, so this sets it in every channel at
 * once. The bottom layer has no Mix node and is named after its map, which renaming an #Image is
 * the job of; it is refused here with #PaintMaterialLayerEditError::IsBottomLayer.
 */
bool BKE_paint_material_layer_rename(Main &bmain,
                                     Material &ma,
                                     int ordinal,
                                     const char *name,
                                     PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Add an empty layer group directly above the layer at \a ordinal, in every channel.
 *
 * A folder to fill, rather than a folder made out of what is already there: nothing moves into it.
 * Its Group Output is left unlinked, which is what "empty" means in this contract -- an unlinked
 * Result is transparent and its Alpha is zero, so the folder contributes nothing until a layer is
 * put in it, exactly like the unlinked bottom of a uniform chain.
 *
 * \param ordinal: the row the group is placed next to; -1 puts it at the top of the stack.
 * \param place: which side of \a ordinal the group lands on. #PaintMaterialLayerMovePlace::Below
 *   on the bottom row is what puts a group under the whole stack; it is refused while the bottom
 *   is still a bare Image Texture (#PaintMaterialLayerEditError::IsBottomLayer), since there is
 *   nothing to blend under. #PaintMaterialLayerMovePlace::Into is not a place a new group takes.
 * \param r_ordinal: when given, receives the ordinal the group row ends up at.
 * \param r_group_tree: when given, receives the node tree the empty group owns -- the tree a
 *   caller that fills this group with copied content replaces, and the one a caller that has to
 *   take the group back out is responsible for deleting.
 */
bool BKE_paint_material_layer_group_add(Main &bmain,
                                        Material &ma,
                                        int ordinal,
                                        PaintMaterialLayerMovePlace place =
                                            PaintMaterialLayerMovePlace::Above,
                                        int *r_ordinal = nullptr,
                                        PaintMaterialLayerEditError *r_error = nullptr,
                                        bNodeTree **r_group_tree = nullptr);

/**
 * Add an empty layer group directly above the layer at \a ordinal that *stands for* \a
 * source_material: the stack lists it under the material's name, with the material's icon and
 * preview. Nothing of the material is brought in -- the group's Group Output stays unlinked, an
 * empty folder composites nothing -- and the material itself is referenced, not changed, so a
 * linked one is fine. What the group eventually does with the material is not this contract's
 * business yet.
 *
 * The reference lives on the group's own tree, where #BKE_paint_material_is_layer_group's marker
 * lives; see #BKE_paint_material_layer_group_material_get.
 *
 * \param ordinal: the row the group is placed next to; -1 puts it at the top of the stack.
 * \param place: which side of \a ordinal it lands on; see #BKE_paint_material_layer_group_add.
 * \param r_ordinal: when given, receives the ordinal the group row ends up at.
 */
bool BKE_paint_material_layer_group_material_add(Main &bmain,
                                                 Material &ma,
                                                 Material &source_material,
                                                 int ordinal,
                                                 PaintMaterialLayerMovePlace place =
                                                     PaintMaterialLayerMovePlace::Above,
                                                 int *r_ordinal = nullptr,
                                                 PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Wrap the layers \a from_ordinal .. \a to_ordinal into a layer group, in every channel.
 *
 * The group is isolated (`08 §2.2`): the layers inside it composite with each other starting from
 * transparency, and the result is laid over what is below exactly as one layer would be. That is
 * why the bottom layer of the range keeps its Mix node *outside* the group -- that node is what
 * blends the group with the stack under it, and its blend mode therefore becomes the group's.
 * Inside, the same layer's map becomes the bottom of the sub-stack.
 *
 * The bottom layer of the stack (ordinal 0) has no Mix node and cannot start a range.
 *
 * \param r_ordinal: when given, receives the ordinal the group row ends up at.
 */
bool BKE_paint_material_layer_group_make(Main &bmain,
                                         Material &ma,
                                         int from_ordinal,
                                         int to_ordinal,
                                         int *r_ordinal = nullptr,
                                         PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Unwrap the group at \a ordinal, putting the layers it holds back into the stack around it.
 *
 * The inverse of #BKE_paint_material_layer_group_make, and it undoes it exactly: the group's Mix
 * node stays and goes back to blending the bottom layer's map, and the rest of the sub-stack is
 * spliced in above it. The group's node tree is left behind with no users for the usual clean-up
 * rules, since another material may still be using it.
 *
 * \param r_layer_num: when given, receives how many layers the group turned back into.
 */
bool BKE_paint_material_layer_group_ungroup(Main &bmain,
                                            Material &ma,
                                            int ordinal,
                                            int *r_layer_num = nullptr,
                                            PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Turn the layer at \a ordinal on or off, in every channel.
 *
 * Implemented by muting the layer's Mix nodes, which is what the UI model reads back as
 * "disabled". That makes this a graph mutation rather than a view setting: it belongs in an undo
 * step and it changes what renders, exactly as toggling the nodes by hand would.
 *
 * The bottom layer has no Mix node of its own and is refused with
 * #PaintMaterialLayerEditError::IsBottomLayer.
 */
bool BKE_paint_material_layer_set_enabled(Main &bmain,
                                          Material &ma,
                                          int ordinal,
                                          bool enable,
                                          PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Tell the stack that a layer's Opacity socket was written directly (the Outliner writes it
 * through #RNA_PaintMaterialLayerOpacity). Tags the socket and refreshes the owning material's
 * evaluated copy -- a bare notifier leaves the viewport on the old value.
 */
void BKE_paint_material_layer_opacity_changed(Main &bmain, bNodeTree &tree, bNodeSocket &socket);

/**
 * The state of \a channel on the row \a ordinal, read from the graph alone. A row this file cannot
 * read as a supported layer, a folder, or a channel without a chain all read as Absent.
 */
PaintMaterialLayerChannelState BKE_paint_material_layer_channel_state_get(Main &bmain,
                                                                          Material &ma,
                                                                          int ordinal,
                                                                          int channel);

/**
 * #BKE_paint_material_layer_channel_state_get for every channel at once, indexed by
 * #eMaterialPaintChannel, from one read of the stack. \a r_states must hold
 * #PAINT_MATERIAL_CHANNEL_NUM items.
 */
void BKE_paint_material_layer_channel_states_get(Main &bmain,
                                                   Material &ma,
                                                   int ordinal,
                                                   MutableSpan<PaintMaterialLayerChannelState>
                                                       r_states);

/**
 * Switch \a channel of the row \a ordinal on or off.
 *
 * Off keeps the map (Disabled). On restores a Disabled map, or gives an Absent channel a map: \a
 * new_map when given (a Material layer's bake), otherwise a fresh one -- transparent for a Paint
 * layer, filled with the layer's colour for a Fill layer. A channel with no chain yet gets one
 * first.
 *
 * \a new_map is a fresh data-block with its one user; ownership passes with the call whatever it
 * returns. Nothing is written on refusal.
 */
bool BKE_paint_material_layer_channel_enabled_set(Main &bmain,
                                                  Material &ma,
                                                  int ordinal,
                                                  int channel,
                                                  bool enable,
                                                  Image *new_map,
                                                  PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Remove the layer at \a ordinal from every channel, closing the chain over it.
 *
 * The layer's Image Texture nodes go with it when nothing else reads them; the #Image data-blocks
 * themselves are left to the usual user-count rules, since a map the user painted is not this
 * function's to delete.
 */
bool BKE_paint_material_layer_remove(Main &bmain,
                                    Material &ma,
                                    int ordinal,
                                    PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Bring every channel forest of \a ma to the same row structure for \a channels.
 *
 * Only path S (a stack already exists): an empty material refuses with NotAStack and the
 * caller takes path E (#BKE_paint_material_layer_add_material_base) instead.
 * A needed channel wired to a non-stack network refuses with ChannelHasUnsupportedSource
 * before anything is written.
 *
 * The whole preflight and each single channel's migration are all-or-nothing. If a later
 * channel's migration fails, the channels already migrated on this call stay in the graph;
 * the caller runs under #OPTYPE_UNDO, so one undo step still takes the partial migration back.
 */
bool BKE_paint_material_layer_channels_ensure(Main &bmain,
                                              Material &ma,
                                              Span<int> channels,
                                              PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Path E: build the first layer of an empty material directly as normalized Mix chains
 * holding \a baked_maps (a single row sharing one marker), stamped with \a kind the way
 * #BKE_paint_material_layer_add stamps it.
 *
 * Ownership of \a baked_maps follows #BKE_paint_material_layer_add: shown maps keep their
 * user, the rest (all of them on refusal) are freed.
 */
bool BKE_paint_material_layer_add_material_base(Main &bmain,
                                                Material &ma,
                                                Span<PaintMaterialLayerChannelImage> baked_maps,
                                                PaintMaterialLayerKind kind,
                                                int *r_ordinal = nullptr,
                                                PaintMaterialLayerEditError *r_error = nullptr);

}  // namespace blender
