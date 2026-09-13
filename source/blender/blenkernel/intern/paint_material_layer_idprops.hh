/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * \file
 * \ingroup bke
 *
 * The one place that knows the `pbr_*` IDProperty schema of paint layers: every key, and the
 * typed get/set for the marker, kind, fill colour, colour tag, group marker, group material and
 * normal-combine marker that live on layer nodes and group trees.
 *
 * `image.cc` stays the owner of the `ImageMaterialSource` bake link, but reads its keys from here
 * too, so a key is spelled exactly once in the tree. The public `BKE_paint_material_layer_*`
 * accessors are thin wrappers over these.
 */

#include "BKE_paint_material_layer_model.hh"

#include "BLI_uuid.h"

namespace blender {

struct ID;
struct IDProperty;
struct Material;
struct bNode;
struct bNodeTree;

namespace bke::paint_layer {

/** The layer's identity marker on every Mix node the layer owns. */
inline constexpr const char *LAYER_MARKER_PROP = "pbr_paint_layer";
/**
 * The marker shared by layer-group folders and the Normal Combine group, told apart by value.
 * One property for both: one place to look to know whether a group node is one of ours.
 */
inline constexpr const char *NODE_GROUP_MARKER_PROP = "pbr_paint_node_group";
/** #NODE_GROUP_MARKER_PROP value of a layer-group folder. */
inline constexpr const char *LAYER_GROUP_MARKER_VALUE = "LAYER_GROUP";
/** #NODE_GROUP_MARKER_PROP value of the Normal Combine group. */
inline constexpr const char *NORMAL_COMBINE_MARKER_VALUE = "NORMAL_COMBINE";
/** Name of the shared Normal Combine group data-block. NOTE: do not translate. */
inline constexpr const char *NORMAL_COMBINE_TREE_NAME = "PBR Normal Combine";
/** Colour tag of a layer-group folder, on the group node. */
inline constexpr const char *LAYER_COLOR_TAG_PROP = "pbr_paint_color_tag";
/** What kind of layer a node is, on the same nodes the layer marker lives on. */
inline constexpr const char *LAYER_KIND_PROP = "pbr_paint_layer_kind";
/** The colour a Fill layer stands for, as a 4-float array. */
inline constexpr const char *LAYER_FILL_COLOR_PROP = "pbr_paint_fill_color";
/** The material a layer group stands for, on the group's own tree. */
inline constexpr const char *LAYER_GROUP_MATERIAL_PROP = "pbr_paint_layer_material";
/** Which part of a correction layer the node stands for, on the correction's own Mix nodes. */
inline constexpr const char *CORRECTION_SECTION_PROP = "pbr_paint_correction_section";
/** What a correction layer applies to the layer under it. */
inline constexpr const char *CORRECTION_EFFECT_PROP = "pbr_paint_correction_effect";
/**
 * The bake link on an #Image: which material, channel, size and node-tree hash it was baked
 * from. Owned by `image.cc` (#ImageMaterialSource); the keys live here so they are spelled once.
 */
inline constexpr const char *BAKE_MATERIAL_PROP = "pbr_bake_material";
inline constexpr const char *BAKE_CHANNEL_PROP = "pbr_bake_channel";
inline constexpr const char *BAKE_SIZE_PROP = "pbr_bake_size";
inline constexpr const char *BAKE_HASH_PROP = "pbr_bake_hash";

/** The node's property group, creating it when missing. */
IDProperty *node_properties_ensure(bNode &node);

bUUID marker_get(const bNode &node);
void marker_set(bNode &node, const bUUID &layer_id);

int color_tag_get(const bNode &node);
void color_tag_set(bNode &node, int color_tag);

PaintMaterialLayerKind kind_get(const bNode &node);
void kind_set(bNode &node, PaintMaterialLayerKind kind);

/** Which part of a correction layer the node stands for; reads as Content when unset. */
PaintMaterialCorrectionSection correction_section_get(const bNode &node);
void correction_section_set(bNode &node, PaintMaterialCorrectionSection section);

/** What a correction layer applies to the layer under it; reads as Paint when unset. */
PaintMaterialCorrectionEffect correction_effect_get(const bNode &node);
void correction_effect_set(bNode &node, PaintMaterialCorrectionEffect effect);

/** Whether the node is a correction layer: #kind_get(node) == Correction. */
bool node_is_correction(const bNode &node);

bool fill_color_get(const bNode &node, float r_color[4]);
void fill_color_set(bNode &node, const float color[4]);

/** Stamp a fresh group tree as a layer-group folder. */
void layer_group_tree_marker_set(bNodeTree &group);
/** Stamp the shared Normal Combine group tree. */
void normal_combine_tree_marker_set(bNodeTree &group);
/** The #NODE_GROUP_MARKER_PROP value on \a tree_id, or null when unmarked. */
const char *node_tree_group_marker_get(const ID &tree_id);

Material *group_material_get(const ID &group_tree_id);
void group_material_set(ID &group_tree_id, Material *material);

}  // namespace bke::paint_layer
}  // namespace blender
