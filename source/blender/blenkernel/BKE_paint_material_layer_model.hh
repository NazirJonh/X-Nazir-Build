/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The paint layer stack as a UI model: one entry per row, read from the material's node graph.
 *
 * The stack has no storage of its own -- it *is* the chain of Mix nodes that
 * #BKE_paint_material_layer_stack_from_material reads back -- so this header only names the
 * vocabulary readers share: the row type, the layer kind, the per-channel states. Mutation lives
 * in #BKE_paint_material_layer_edit.hh, pixel evaluation in #BKE_paint_material_composite.hh.
 */

#include <cstdint>

#include <string>

#include "BLI_map.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "RNA_types.hh"

namespace blender {

struct Image;
struct Main;
struct Material;
struct bNode;
struct bNodeTree;
struct ID;

/**
 * How a layer combines with what is below it.
 *
 * Deliberately a short list: it is the set of Mix node blend modes that
 * #BKE_paint_material_composite_stack_from_material knows how to reproduce byte-exactly. A chain
 * using any other mode is not expressible as a stack and falls to the bake instead of being
 * approximated here.
 */
enum class CompositeBlend : int8_t {
  Mix = 0,
  Multiply,
  Overlay,
  Add,
  /**
   * Combine two tangent-space normal maps, rather than blending their encoded bytes.
   *
   * Encoded normals are not colours: averaging two of them channel by channel flattens the
   * relief instead of laying one over the other, which is why every layer stack that supports
   * normals has an operation of its own for it. This is the whiteout blend -- the detail map's
   * slope added to the base map's, renormalized -- which is what "overlay"/"add" means for a
   * normal layer.
   *
   * Nothing in a plain Mix chain selects this: a Mix node really does interpolate the encoded
   * values, and reproducing it any other way would make the composite disagree with the render.
   * It exists for the graph shapes that genuinely combine normals.
   */
  NormalCombine,
};

/**
 * What a layer *is*, as opposed to how one was made.
 *
 * Stored as an id-property on the layer's Mix nodes -- on the group's own tree for a group -- the
 * same way #BKE_paint_material_layer_marker_get stores identity. A layer with no marker at all
 * reads as #Paint, which is what a stack authored before this contract, or wired by hand in the
 * Shader Editor, actually is.
 */
enum class PaintMaterialLayerKind : int8_t {
  Paint = 0,
  Fill,
  Material,
  /**
   * A child of a layer: its Mix nodes sit between the layer's base and the layer's own Mix node
   * (spec 18 §4.1).
   */
  Correction,
  /* The enum stays open: a kind this build does not know reads back as #Paint. */
};

/**
 * Which part of a correction layer the row's UI shows: the adjustment it applies, or the mask
 * that limits where it applies. Stored next to the kind on the correction's own Mix nodes.
 */
enum class PaintMaterialCorrectionSection : int8_t { Content = 0, Mask = 1 };

/** What a correction layer applies to the layer it hangs under. */
enum class PaintMaterialCorrectionEffect : int8_t { Paint = 0, Fill };

/** How one channel of one stack row stands; see the spec's invariants I1 and I2. */
enum class PaintMaterialLayerChannelState : int8_t {
  /** No map: the row keeps its Mix and Multiply, its coverage is unlinked and zero. */
  Absent = 0,
  /** A map feeds the row, its coverage comes from the map's alpha or the layer's mask. */
  Enabled,
  /** The map stays on its (muted) node, the coverage is unlinked and zero. */
  Disabled,
};

/**
 * Where the ordinals of layers held inside a group start.
 *
 * Rows at the top level are numbered by their position in the channel chain, so an ordinal from
 * the UI names the same layer to the graph editor. Rows inside a group have no position in that
 * chain at all, so they are numbered from a range of their own: the number stays unique for the
 * tree store, and an ordinal at or above this base is recognizable as "inside a group" by code
 * that can only act on the chain.
 */
constexpr int PAINT_LAYER_GROUP_CHILD_ORDINAL_BASE = 1024;

/**
 * One correction of a layer row, as the UI shows it (spec 18 §4.5): a child row carrying either
 * the adjustment a layer applies or the mask limiting where it applies.
 *
 * Like a layer row it preserves node identity while staying independent from the pixel evaluator.
 * Every channel owns its own nodes for the same correction -- they share the marker -- so the
 * per-channel maps are keyed by #eMaterialPaintChannel, while the row's maps are found by its
 * marker rather than by the position a channel's chain happens to give it.
 */
struct PaintMaterialLayerCorrectionEntry {
  /** The correction's identity, shared by every channel's nodes for it. */
  bUUID marker = {};
  /** Which part of the parent layer this row adjusts. */
  PaintMaterialCorrectionSection section = PaintMaterialCorrectionSection::Content;
  PaintMaterialCorrectionEffect effect = PaintMaterialCorrectionEffect::Paint;
  /** The row's display name: the node's label, or "Correction" when the user set none. */
  std::string name;
  /**
   * The #bNode::label of the correction's Mix in the reference channel, handed out the way a
   * layer row hands its own out: a pointer into the node, so a UI text field can type straight
   * into it rather than into a copy the next rebuild discards. Never null.
   */
  char *label = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  bool enabled = true;
  bool supported = true;
  /** Per #eMaterialPaintChannel: the correction's map, Disabled ones included. */
  Map<int, Image *> channel_images;
  /** Per #eMaterialPaintChannel: the Opacity socket as #RNA_PaintMaterialLayerOpacity. */
  Map<int, PointerRNA> channel_factor_props;
  /** Per #eMaterialPaintChannel: the node carrying `blend_type`. */
  Map<int, PointerRNA> channel_blend_props;
  /** Bit per #eMaterialPaintChannel whose map is kept but switched off (Disabled). */
  uint32_t disabled_channels_mask = 0;
};

/**
 * One paint layer as it appears in a UI. Unlike #PaintMaterialCompositeImageLayer, this preserves
 * the node identity and editable socket while remaining independent from the pixel evaluator.
 */
struct PaintMaterialLayerStackEntry {
  int16_t ordinal = 0;
  int32_t node_id = 0;
  const bNodeTree *owner_tree = nullptr;
  uint32_t material_sid = 0;
  uint32_t tree_sid = 0;
  int depth = 0;
  int32_t parent_node_id = 0;
  bool is_group = false;
  /** The row is a bare Image Texture wired straight into the channel, not a blended layer. */
  bool is_bare_base = false;
  /**
   * The layer's identity, nil for a bare base (it has no Mix node to carry one). Stable across an
   * edit that moves the row -- unlike #ordinal, which is only ever a position -- so a caller that
   * has to recognize the same row again after one, such as the Outliner keeping it open or
   * selected, can key off this instead.
   */
  bUUID marker = {};
  /** The group's own node tree, when the row is a group; null otherwise. */
  const bNodeTree *group_tree = nullptr;
  std::string name;
  /**
   * The layer's own name as editable storage, or null when the row has none.
   *
   * #name is what the row reads as, which falls back to the layer's map or its node when the user
   * has set no name; this is the #bNode::label that name is set through, handed out so that a UI
   * text field can type straight into it rather than into a copy that the next rebuild discards.
   * Null for a bare base, which has no Mix node to carry a label, and for an unsupported row.
   */
  char *label = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  float opacity = 1.0f;
  bool enabled = true;
  bool has_mask = false;
  /** Color tag for group folders (0-7), or -1 for no tag. */
  int8_t color_tag = -1;
  /** What the row is; #PaintMaterialLayerKind::Paint for a row that carries no kind marker. */
  PaintMaterialLayerKind kind = PaintMaterialLayerKind::Paint;
  /** Meaningful only for a Fill row. */
  float fill_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bool supported = true;
  const char *unsupported_reason = nullptr;
  /** Per #eMaterialPaintChannel: the row's map, Disabled ones included (see
   * #disabled_channels_mask). */
  Map<int, Image *> channel_images;
  /** Per #eMaterialPaintChannel: the Opacity socket as #RNA_PaintMaterialLayerOpacity. */
  Map<int, PointerRNA> channel_factor_props;
  /** Per #eMaterialPaintChannel: the node carrying `blend_type`; no entry for Normal. */
  Map<int, PointerRNA> channel_blend_props;
  /** Bit per #eMaterialPaintChannel whose map is kept but switched off (Disabled). */
  uint32_t disabled_channels_mask = 0;
  /** The corrections hanging on the row's content stack, bottom to top (spec 18 §4.5). */
  Vector<PaintMaterialLayerCorrectionEntry> content_corrections;
  /** The corrections limiting where the row applies, bottom to top. */
  Vector<PaintMaterialLayerCorrectionEntry> mask_corrections;
  /**
   * Bit per #eMaterialPaintChannel the row actually paints into (spec 18 I2'): its base map is on
   * in the channel, or one of its corrections is.
   */
  uint32_t contributing_channels_mask = 0;
};

/**
 * Build the paint-layer UI model from all channels of \a ma's node graph, bottom to top.
 *
 * This is a structure-only traversal: it does not acquire image buffers and is not a conversion of
 * the evaluator's model, which intentionally lacks node and tree identity.
 */
bool BKE_paint_material_layer_stack_from_material(
    const Main &bmain, const Material &ma, Vector<PaintMaterialLayerStackEntry> &r_entries);

/**
 * Whether \a ma resolves to a paint layer stack at all.
 *
 * Answers the same question as #BKE_paint_material_layer_stack_from_material without aggregating
 * the channels or reaching into #Main, for callers that only need to decide whether to list the
 * material -- the Outliner object overview asks this once per object on every rebuild.
 */
bool BKE_paint_material_has_layer_stack(const Material &ma);

/**
 * Whether \a node is a paint layer group: a folder of layers, composited on transparency.
 *
 * Such a group is an ordinary layer as far as the stack around it is concerned -- its `Result` is
 * the top socket of the Mix node above it and its `Alpha` that node's factor -- and a sub-stack as
 * far as the stack inside it is concerned. See `08 §2.2`.
 */
bool BKE_paint_material_is_layer_group(const bNode &node);

/**
 * The material the paint layer group whose tree is \a group_tree_id stands for, or null.
 *
 * The reference lives in an IDProperty on the group's own tree, next to the marker that names it
 * a layer group. A group standing for a material is what a material dropped on the stack
 * becomes: a placeholder the stack lists with the material's name, icon and preview, holding
 * nothing of its own yet.
 */
Material *BKE_paint_material_layer_group_material_get(const ID &group_tree_id);

/**
 * Make the paint layer group whose tree is \a group_tree_id stand for \a material, replacing
 * whatever it stood for. The user count follows the reference, whichever way it moves.
 *
 * A null \a material takes the reference away again, leaving a plain group: the reference is the
 * kind of state a group can lose -- the material it stood for being replaced or dropped -- and a
 * setter that could only ever put one on would leave no way back.
 */
void BKE_paint_material_layer_group_material_set(ID &group_tree_id, Material *material);

}  // namespace blender
