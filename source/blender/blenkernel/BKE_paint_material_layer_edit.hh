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
 * are refused here rather than half-implemented -- see #PaintMaterialLayerEditError::IsBottomLayer.
 */

#include <cstdint>

#include "BLI_uuid.h"

namespace blender {

struct Main;
struct Material;
struct bNode;

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

/** What the map of a newly added layer starts out as. */
enum class PaintMaterialLayerAddType : int8_t {
  /** A fully transparent map. Painting on it is what makes the layer show. */
  Image = 0,
  /**
   * A map pre-filled with a flat color, so the layer covers what is below it right away.
   *
   * Deliberately an ordinary Image Texture rather than a constant node: the top socket of a layer
   * is contractually a #SH_NODE_TEX_IMAGE, and a fill that is a real map stays paintable.
   */
  Fill,
};

/** How a layer is created. Defaults describe "an empty layer on top of the stack". */
struct PaintMaterialLayerAddParams {
  PaintMaterialLayerAddType type = PaintMaterialLayerAddType::Image;
  /**
   * Where the new layer ends up. -1 puts it on top; 0 is refused, since the bottom layer is a bare
   * Image Texture rather than a Mix node (#PaintMaterialLayerEditError::IsBottomLayer).
   */
  int ordinal = -1;
  int image_size = 1024;
  /** Used by #PaintMaterialLayerAddType::Fill; non-color channels take the red component. */
  float fill_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  /** Name for the layer; null uses the channel-scoped default the paint code already uses. */
  const char *name = nullptr;
};

/**
 * Add a paint layer to \a ma, in every channel that is already wired as a stack.
 *
 * A material with no stack at all gets one: a single Image Texture on Base Color, which is the
 * bottom layer. The other channels are left to #BKE_paint_material_images_ensure_writable, which
 * creates a channel's map the first time a brush actually writes to it.
 *
 * Transactional like the rest of this file: every node is created and validated before the first
 * link is rewritten, so a channel that cannot take the layer leaves the graph untouched.
 *
 * \param r_ordinal: when given, receives the position the new layer ended up at.
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
 */
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
 * \return whether anything was converted.
 */
bool BKE_paint_material_layer_bottom_normalize(Main &bmain, Material &ma);

bool BKE_paint_material_layer_move(Main &bmain,
                                   Material &ma,
                                   int from_ordinal,
                                   int anchor_ordinal,
                                   PaintMaterialLayerMovePlace place,
                                   PaintMaterialLayerEditError *r_error = nullptr);

/**
 * Give the layer at \a ordinal a mask: one image driving the Factor of its Mix nodes in every
 * channel.
 *
 * \note A layer without a mask takes its Factor from its own map's Alpha, so adding a mask
 * *replaces* what covers the layer rather than multiplying with it. That is a consequence of the
 * node contract -- a layer's Factor is either an image or nothing, never an expression -- so the
 * new mask starts out white and the layer therefore covers everything below it until the mask is
 * painted. Callers that show this to a user should say so.
 *
 * Fails with #PaintMaterialLayerEditError::IsBottomLayer for the bottom layer, which has no Mix
 * node to mask, and does nothing when the layer already has one.
 */
bool BKE_paint_material_layer_mask_add(Main &bmain,
                                       Material &ma,
                                       int ordinal,
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
 * Copy the layer at \a ordinal, maps and all, and put the copy directly above it.
 *
 * The copies are new #Image data-blocks with the same pixels: a duplicated layer that shared its
 * maps would not be a copy, it would be the same layer listed twice.
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
 * \param ordinal: the row to sit above; -1 puts the group at the top of the stack.
 * \param r_ordinal: when given, receives the ordinal the group row ends up at.
 */
bool BKE_paint_material_layer_group_add(Main &bmain,
                                        Material &ma,
                                        int ordinal,
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

}  // namespace blender
