/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The paint-layer generator: `Material::paint_layers` (the DNA description) becomes a node tree.
 *
 * `BKE_paint_layers_tree_build` is the topology half: interface, per-layer nodes and the links between
 * them, with no #Main involved. `BKE_paint_layers_regenerate` is the #Main-side half: it owns the
 * generated group, the instance node in the material's embedded tree, and the routing into the
 * Principled BSDF.
 *
 * Everything declared here is private to the generator's translation units and its tests; the
 * public entry points live in `BKE_paint_layers_generate.hh`.
 *
 * A tree under construction has no topology cache. Code here never asks a socket for its
 * `owner_node()` or its `is_directly_linked()`: the owning node is passed along with the socket
 * and links are read from `bNodeTree::links` (see #uv_readers_find, #normal_remap_nodes_add).
 */

#include <cstddef>
#include <string>

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_material_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_kind.hh"

namespace blender {

struct bNode;
struct bNodeSocket;
struct bNodeTree;
struct bNodeTreeInterface;
struct bNodeTreeInterfaceSocket;
struct IDProperty;
struct Image;
struct Main;
struct Material;
struct MaterialPaintChannelInfo;
struct MaterialPaintLayer;

namespace bke::paint_layers {

/**
 * The interface name of the `Source <channel>` input the channel \a info describes: the name the
 * builder adds the interface socket under, the wiring matches it by (through
 * #instance_input_find), and the hash walks read. One spelling, so a new lookup site cannot drift
 * back to matching an identifier against it.
 */
void source_input_name_get(const MaterialPaintChannelInfo &info, char (&r_name)[64]);

/** On a generated tree: the uid of the material that owns it, so a copy never adopts its source's
 * groups and a foreign tree is never overwritten. */
inline constexpr const char *TREE_OWNER_PROP = "pbr_paint_layers_owner";
/** On the instance node in the material's embedded tree: the same owner uid. */
inline constexpr const char *INSTANCE_OWNER_PROP = "pbr_paint_layers_instance";
/**
 * On the Mix Shader and Emission nodes the viewport paint pass preview installs in the material's
 * embedded tree: 1 marks them as that viewer, so a user's own Mix or Emission is never mistaken for
 * it. A flag rather than an owner uid: the viewer lives in the material's own tree, which a copy
 * carries with it.
 */
inline constexpr const char *VIEW_PASS_MIX_PROP = "pbr_paint_view_pass_mix";
inline constexpr const char *VIEW_PASS_EMISSION_PROP = "pbr_paint_view_pass_emission";
/** On a layer's own node group: the marker of the layer it holds. The tree also carries
 * #TREE_OWNER_PROP of the material, so a copy never adopts the source's groups. */
inline constexpr const char *TREE_LAYER_PROP = "pbr_paint_layers_layer_tree";
/**
 * On a Material row's source group: the marker of the row it was made for. The tree also carries
 * #TREE_OWNER_PROP of the material; its topology hash words hold the content hash of the source it
 * copies, so the sync knows when the source moved.
 */
inline constexpr const char *TREE_SOURCE_ROW_PROP = "pbr_paint_layers_source_row";
/**
 * On a private copy of a group a Material row's source reaches its Principled BSDF through (see
 * #source_group_fill): owned by the instance node of its parent copy and deleted with it
 * (#source_group_paths_prune). The tree also carries #TREE_OWNER_PROP of the material.
 */
inline constexpr const char *TREE_SOURCE_PATH_PROP = "pbr_paint_layers_source_path";
/** On a generated group interface input: how its value is read from the description. */
inline constexpr const char *INPUT_ROLE_PROP = "pbr_paint_layers_role";
/** On a generated group interface input: the marker of the layer it stands for. */
inline constexpr const char *INPUT_MARKER_PROP = "pbr_paint_layers_layer";
/** On a generated group interface input: the channel the value belongs to. */
inline constexpr const char *INPUT_CHANNEL_PROP = "pbr_paint_layers_channel";
/**
 * On a layer's own node group: its topology hash, low and high 32-bit words. The factory compares it
 * with #paint_layers_layer_topology_hash and skips the rebuild of a group whose hash is unchanged.
 * A missing or unreadable pair is treated as changed, so a file written before this feature never
 * keeps a stale group.
 */
inline constexpr const char *TREE_TOPOLOGY_LOW_PROP = "pbr_paint_layers_topology";
inline constexpr const char *TREE_TOPOLOGY_HIGH_PROP = "pbr_paint_layers_topology_hi";
/** On the root generated tree: the hash of the rows in order and their per-channel participation.
 * When it matches the stored one the root is left alone and only its groups' values are synced. */
inline constexpr const char *TREE_ROOT_TOPOLOGY_LOW_PROP = "pbr_paint_layers_root_topology";
inline constexpr const char *TREE_ROOT_TOPOLOGY_HIGH_PROP = "pbr_paint_layers_root_topology_hi";
/**
 * On the root generated tree: 1 when any row or correction was built with the runtime blend group.
 * The tier itself is runtime-only; this dirty bit only tells a file's loader that the saved tree
 * has to be rebuilt into static Mix nodes before it is used.
 */
inline constexpr const char *TREE_ROOT_BLEND_DYNAMIC_PROP = "pbr_paint_blend_dynamic";
/** On a source group: the source's structure hash (#BKE_paint_layers_source_material_structure_hash)
 * it was filled from. With it unchanged, a source edit only moved socket values, which the group
 * takes in place (#source_group_values_sync). */
inline constexpr const char *TREE_SOURCE_STRUCTURE_LOW_PROP = "pbr_paint_layers_source_structure";
inline constexpr const char *TREE_SOURCE_STRUCTURE_HIGH_PROP =
    "pbr_paint_layers_source_structure_hi";

/**
 * On a generated layer group or the root: 1 once #NODE_KEEP_INACTIVE_BRANCH was set on its Mix
 * nodes. A node added by a later rebuild lacks the flag, so a rebuilt tree is swept again; a tree
 * that was only kept is not (the flag survived with its nodes).
 */
inline constexpr const char *TREE_PRUNE_GUARD_PROP = "pbr_paint_layers_prune_guard";

/**
 * On a generated Image Texture node of a row's channel: `B:<row marker>:<channel>`, the map the
 * node shows. The kind of node does not change when the row swaps one image for another, so the
 * value sync writes the new image pointer instead of rebuilding the group.
 */
inline constexpr const char *MAP_NODE_KEY_PROP = "pbr_paint_layers_map";

/**
 * On a generated ColorRamp node: `R:<row marker>:<channel>`, with -1 for the row's shared colour
 * ramp and the channel otherwise. A ramp whose shader shape did not change is copied into the
 * node's storage in place instead of rebuilding the group.
 */
inline constexpr const char *RAMP_NODE_KEY_PROP = "pbr_paint_layers_ramp";

/** On a row's opacity input: set (to 1) exactly when the row was built from a bake of the whole
 * row, so that input holds the row's visibility alone -- the bake already holds its opacity -- and
 * the value sync can tell a group built that way from one built live or over a content bake. */
inline constexpr const char *INPUT_SUBSTITUTED_PROP = "pbr_paint_layers_baked";

/** #INPUT_ROLE_PROP value of a layer's opacity (enabled already folded in). A content correction's
 * opacity input carries the same role with the correction's own marker. */
inline constexpr const char *ROLE_OPACITY = "opacity";
/**
 * #INPUT_ROLE_PROP value of the scale of a row's Height record (`1 + delta`): the height modes'
 * delta-space step multiplies the record's distance from the neutral level by it. A value, so a
 * scale edit never rebuilds the chain.
 */
inline constexpr const char *ROLE_HEIGHT_SCALE = "height_scale";
/**
 * #INPUT_ROLE_PROP value of the float input that picks the content alpha model of a (row, channel):
 * 1 for a Mix row, whose alpha is mixed by the row's factor, and 0 for every other mode, which
 * keeps the alpha below. A value, so changing the blend never rewires the alpha chain.
 */
inline constexpr const char *ROLE_BLEND_IS_MIX = "blend_is_mix";
/**
 * On a generated Mix node whose blend_type follows the description: `B:<marker>:<channel>` for a
 * row or a content correction, `M:<marker>` for a mask item's coverage. The sync updates these
 * nodes in place, so a blend edit rebuilds no group.
 */
inline constexpr const char *BLEND_NODE_KEY_PROP = "pbr_paint_layers_blend_key";
/** On a layer group: how many nodes carry #BLEND_NODE_KEY_PROP. A mismatch means a node was
 * deleted or duplicated (a hand-edited tree), and the group is rebuilt rather than left without a
 * blend. */
inline constexpr const char *BLEND_NODE_COUNT_PROP = "pbr_paint_layers_blend_nodes";
/**
 * On a generated RGB Curves node: the marker of the row (a correction, an adjustment or a mask
 * item) whose curve mapping the node carries a copy of in its storage. Unlike a blend, the curves'
 * shape changes no node set, so the sync updates the storage in place and the shape is deliberately
 * out of the topology hash; this key is what lets the sync find the node without a rebuild. The
 * value is the plain UUID string of the row's marker, parsed back with #BLI_uuid_parse_string.
 */
inline constexpr const char *CURVES_NODE_KEY_PROP = "pbr_paint_layers_curves_key";
/** On a layer group: how many nodes carry #CURVES_NODE_KEY_PROP, the same hand-edit guard the
 * blend nodes carry #BLEND_NODE_COUNT_PROP for. */
inline constexpr const char *CURVES_NODE_COUNT_PROP = "pbr_paint_layers_curves_nodes";
/**
 * On the Bump and Normal Map nodes of the material's own tree that the stack owns: which of the two
 * the node is (#RESPONSE_NODE_BUMP, #RESPONSE_NODE_NORMAL_MAP), so a user's own Bump or Normal Map
 * is never mistaken for them the way a "first node of the type" search would. The response values
 * of the owned nodes live on the material, and the sync below writes them into the sockets.
 */
inline constexpr const char *RESPONSE_NODE_KEY_PROP = "pbr_paint_layers_response_key";
inline constexpr const char *RESPONSE_NODE_BUMP = "bump";
inline constexpr const char *RESPONSE_NODE_NORMAL_MAP = "normal_map";
/**
 * On the material's own node tree: the #bNode::identifier of every response node the generator
 * minted, comma-separated. The revert removes exactly these -- see
 * #response_nodes_created_record.
 */
inline constexpr const char *RESPONSE_NODE_CREATED_PROP = "pbr_paint_layers_response_created";
/** #INPUT_ROLE_PROP value of the constant of a channel that carries no map. */
inline constexpr const char *ROLE_FILL = "fill";
/** #INPUT_ROLE_PROP value of that constant's alpha: the content alpha a map-less row lays, which
 * no colour socket can hand over on its own. */
inline constexpr const char *ROLE_FILL_ALPHA = "fill_alpha";
/** #INPUT_ROLE_PROP value of the plain value a Material row's source supplies in a channel: an
 * RGBA input of the row's group, filled from the source and kept current by the value sync, so
 * editing the value in the source never rebuilds the row. */
inline constexpr const char *ROLE_LIVE_CONSTANT = "live_constant";
/** #INPUT_ROLE_PROP value of a Material row's coverage when the source's Alpha is a plain value
 * (a float input, the same way). */
inline constexpr const char *ROLE_LIVE_COVERAGE = "live_coverage";
/** #INPUT_ROLE_PROP value of a parameter of a kind (the marker is the row's own); the parameter is
 * named by #INPUT_PARAM_PROP. */
inline constexpr const char *ROLE_PARAM = "param";
/** On a #ROLE_PARAM input: the position of the parameter in its kind's schema. */
inline constexpr const char *INPUT_PARAM_PROP = "pbr_paint_layers_param";
/** #INPUT_ROLE_PROP value of a row's base mask constant (the marker is the owner row's). */
inline constexpr const char *ROLE_MASK_VALUE = "mask_value";
/** #INPUT_ROLE_PROP value of a row's base mask opacity (enabled already folded in). */
inline constexpr const char *ROLE_MASK_OPACITY = "mask_opacity";
/** #INPUT_ROLE_PROP value of a mask item's contrast (the multiplier itself, the stored offset plus
 * one). The marker is the owner row's for the base mask, the item's own for a correction. */
inline constexpr const char *ROLE_MASK_CONTRAST = "mask_contrast";
/** #INPUT_ROLE_PROP value of a row's base mask switch: 1 enabled, 0 off, which hides the whole
 * mask stack (the owner row's marker). Apart from the opacity, which only weighs the base. */
inline constexpr const char *ROLE_MASK_ENABLED = "mask_enabled";
/** #INPUT_ROLE_PROP value of a row's UV mapping offset (a Vector input holding offset, 0). The
 * marker is the row's own, whether a layer, a correction or a mask item. */
inline constexpr const char *ROLE_MAPPING_OFFSET = "mapping_offset";
/** #INPUT_ROLE_PROP value of a row's UV mapping scale (a Vector input holding scale, 1). */
inline constexpr const char *ROLE_MAPPING_SCALE = "mapping_scale";
/** #INPUT_ROLE_PROP value of a row's UV mapping rotation (a Vector input holding 0, 0, angle). */
inline constexpr const char *ROLE_MAPPING_ROTATION = "mapping_rotation";
/**
 * #INPUT_ROLE_PROP value of a row's UV mirror fold center (a Vector input holding center, 0). Only
 * built while the row's mapping folds an axis; the folded axes are the chain's shape, not a value.
 */
inline constexpr const char *ROLE_MAPPING_MIRROR_CENTER = "mapping_mirror_center";
/** #INPUT_ROLE_PROP value of a Gradient row's UV-space line start (a Vector input, z unused). */
inline constexpr const char *ROLE_GRADIENT_START = "gradient_start";
/** #INPUT_ROLE_PROP value of a Gradient row's UV-space line end (a Vector input, z unused). */
inline constexpr const char *ROLE_GRADIENT_END = "gradient_end";
/** #INPUT_ROLE_PROP value of a Gradient row's midpoint (a Float input). */
inline constexpr const char *ROLE_GRADIENT_MIDPOINT = "gradient_midpoint";
/** #INPUT_ROLE_PROP value of a Gradient row's type (a Float input holding the enum value). */
inline constexpr const char *ROLE_GRADIENT_TYPE = "gradient_type";
/** #INPUT_ROLE_PROP value of a Gradient row's repeat mode (a Float input). */
inline constexpr const char *ROLE_GRADIENT_REPEAT = "gradient_repeat";
/** #INPUT_ROLE_PROP value of a generator row's placement offset (a Vector input holding
 * `offset_x, offset_y, 0`), the first half of the mapping chain its pattern reads. The marker is the
 * generating row's own, whether a layer, a correction or a mask item. */
inline constexpr const char *ROLE_GENERATOR_OFFSET = "generator_offset";
/** #INPUT_ROLE_PROP value of a generator row's placement scale (a Vector input holding
 * `1, scale_y, 1`): the Y ratio the pattern is sheared by before the turn. */
inline constexpr const char *ROLE_GENERATOR_SCALE = "generator_scale";
/** #INPUT_ROLE_PROP value of a generator row's placement turn (a Vector input holding
 * `0, 0, radians`). The schema stores degrees; the Vector Rotate node reads radians. */
inline constexpr const char *ROLE_GENERATOR_ROTATION = "generator_rotation";
/**
 * #INPUT_ROLE_PROP value of a Color Balance filter's folded triple (a Vector input holding the
 * three numbers the MULTIPLY_ADD / MAXIMUM / POWER chain reads: the linear slope, its offset and
 * the power's exponent). The method the payload carries is not part of the graph — only the folded
 * A, B and E are — and the position of the triple (#INPUT_PARAM_PROP: 0, 1 or 2) names which one.
 */
inline constexpr const char *ROLE_COLOR_BALANCE = "color_balance";
inline constexpr const char *ROLE_CHANNEL_MIXER = "channel_mixer";
/** #INPUT_ROLE_PROP value of a projected row's frame centre (a Vector input). */
inline constexpr const char *ROLE_PROJECTION_CENTER = "projection_center";
/** #INPUT_ROLE_PROP value of a projected row's frame extents (a Vector input, normalized). */
inline constexpr const char *ROLE_PROJECTION_SIZE = "projection_size";
/** #INPUT_ROLE_PROP value of a projected row's frame rotation (a Vector input, XYZ Euler radians).
 */
inline constexpr const char *ROLE_PROJECTION_ROTATION = "projection_rotation";
/** #INPUT_ROLE_PROP value of a projected row's face blend (a Float input, 0..1). */
inline constexpr const char *ROLE_PROJECTION_BLEND = "projection_blend";
/** #INPUT_ROLE_PROP value of a projected row's clip toggle (a Float input, 0 or 1). */
inline constexpr const char *ROLE_PROJECTION_CLIP = "projection_clip";
/** #INPUT_ROLE_PROP value of a projected row's depth-slab toggle (a Float input, 0 or 1): one
 * limits the frame factor to the axis' slab, zero leaves the depth unbounded. */
inline constexpr const char *ROLE_PROJECTION_DEPTH = "projection_depth";
/**
 * #INPUT_ROLE_PROP value of a projected row's facing weight (a Vector input): the X and Y hold the
 * scale and offset of the clamped Multiply Add that turns the frame-space normal component along
 * the axis into the 0..1 hermite weight, and the Z holds the toggle. Baked out of the facing limit
 * so a slider drag never rebuilds.
 */
inline constexpr const char *ROLE_PROJECTION_FACING = "projection_facing";
/**
 * #INPUT_ROLE_PROP value of a projected row's facing direction (a Vector input): the normalized
 * world-space direction the world normal reads against, baked out of the controller's transform
 * and the inline rotation. Unused while the row has no controller.
 */
inline constexpr const char *ROLE_PROJECTION_FACING_DIR = "projection_facing_dir";
/**
 * #INPUT_ROLE_PROP value of a projected row's image-aspect fit (a Vector input): the X and Y scale
 * the frame's two footprint coordinates before the 0.5 offset, the Z is unused. (1, 1) when the
 * fit is off or the row reads no image.
 */
inline constexpr const char *ROLE_PROJECTION_ASPECT = "projection_aspect";
/**
 * On a projected row's group input: the tiling of the map over the frame, read from the row's
 * mapping scale (X for the map's U, Y for its V; one while it is zero). It multiplies only the UV,
 * never the frame factor, so the footprint stays the frame and the map repeats inside it.
 */
inline constexpr const char *ROLE_PROJECTION_TILING = "projection_tiling";
/**
 * On a source-group wrapper's interface input: the row mapping value relayed into the wrapper.
 * The same three roles travel from the row group into the wrapper instance and down every private
 * group copy on the path to the source's Principled BSDF, so the Mapping nodes built inside the
 * wrapper read the row's values without a rebuild when a slider moves.
 */
inline constexpr const char *SOURCE_GROUP_ROLE_MAPPING_OFFSET = "source_mapping_offset";
inline constexpr const char *SOURCE_GROUP_ROLE_MAPPING_SCALE = "source_mapping_scale";
inline constexpr const char *SOURCE_GROUP_ROLE_MAPPING_ROTATION = "source_mapping_rotation";
/** #INPUT_ROLE_PROP value of the wrapper's row-mapping mirror center input (a Vector, z unused). */
inline constexpr const char *SOURCE_GROUP_ROLE_MAPPING_MIRROR_CENTER =
    "source_mapping_mirror_center";
/** Prefix of the wrapper's row-mapping value inputs. */
inline constexpr const char *SOURCE_GROUP_MAPPING_INPUT_PREFIX = "Mapping ";
/** #INPUT_ROLE_PROP value of one Color ID colour's enable flag (the marker is the map-coverage
 * item's); the entry is named by #INPUT_ID_COLOR_PROP. */
inline constexpr const char *ROLE_ID_COLOR_ENABLED = "id_color_enabled";
/** #INPUT_ROLE_PROP value of one Color ID colour (an RGBA input in the map's raw data space). */
inline constexpr const char *ROLE_ID_COLOR = "id_color";
/** #INPUT_ROLE_PROP value of one Color ID colour's own tolerance (Advanced mode). */
inline constexpr const char *ROLE_ID_COLOR_TOLERANCE = "id_color_tolerance";
/** On a #ROLE_ID_COLOR / #ROLE_ID_COLOR_ENABLED input: the position of the colour in the item's
 * #MaterialPaintLayer::id_colors list, so the value sync finds it without storing anything else. */
inline constexpr const char *INPUT_ID_COLOR_PROP = "pbr_paint_layers_id_color";

bNode *instance_find(const Material &ma, const bUUID &owner_uid);
bNode *material_output_find(Material &ma);
bNode *mix_node_add(bNodeTree &tree, int ramp_blend, float location_x, float location_y);
/** The stack's own Normal Map node: one the generator placed (#RESPONSE_NODE_KEY_PROP), or a new
 * one. A source-graph material never hands back the author's node of the same type -- their chain
 * is adopted into the stack's `Source Normal` input, not re-fed. */
bNode *normal_map_ensure(Material &ma);
/** The stack's own Bump node, under the same ownership rule as #normal_map_ensure. */
bNode *bump_ensure(Material &ma);
/**
 * The Bump or Normal Map node of \a ma's own tree the stack marked as its own
 * (#RESPONSE_NODE_KEY_PROP), or null. Never creates and never adopts; the generator's
 * #bump_ensure/#normal_map_ensure do that at regeneration. The mutable overload is what the value
 * sync writes through; readers take the const one.
 */
bNode *response_node_find(Material &ma, const char *key);
const bNode *response_node_find(const Material &ma, const char *key);
/**
 * Mark the Bump and Normal Map the stack wired but never marked as its own. Run before a rebuild
 * re-creates the instance: the stack-wired test reads the links from the instance's Result outputs,
 * which the new interface drops, so adopting afterwards would find nothing and mint a second node.
 */
void response_nodes_adopt(Material &ma);

/**
 * Whether \a node carries the stack's ownership marker (#RESPONSE_NODE_KEY_PROP): a Bump or Normal
 * Map the generator minted, or one it adopted because the stack itself had wired it. The one
 * predicate behind the wiring's "never re-aim what the stack feeds" (#source_input_adopt) and the
 * release's stack-owned links, so the two cannot disagree about what belongs to the stack. The
 * generated instance is checked separately by the callers that hold it; it carries no marker.
 *
 * The marker alone does not decide what the revert removes: a node the user duplicated from a
 * helper carries the copied marker but is theirs all the same, and so is one the stack adopted
 * from their graph. The revert takes only the nodes the generator minted
 * (#response_nodes_created_record).
 */
bool node_is_generator_owned(const bNode &node);

/**
 * The identifiers (`bNode::identifier`, comma-separated) of the response nodes the generator minted
 * in the material's embedded tree, recorded on that tree as private ID properties -- the same
 * bookkeeping pattern as the original-links record. The revert removes the minted nodes and
 * nothing else of their kind: a duplicate the user made carries the copied
 * #RESPONSE_NODE_KEY_PROP but an identifier of its own, and a node the stack adopted from the
 * user's graph was minted by the user -- neither is the generator's to delete. `bNode::identifier`
 * names a node across save and load and a material copy; a duplicate of a recorded node is minted
 * an identifier of its own and so stays out of the record.
 */
void response_nodes_created_record(bNodeTree &tree, const bNode &node);
/** Whether \a node is one #response_nodes_created_record wrote down. */
bool response_node_is_created(const bNodeTree &tree, const bNode &node);
/** Drop the record; the revert calls it once the nodes it named are removed. */
void response_nodes_created_clear(bNodeTree &tree);

/**
 * The response values the Normal & Height panel edits: one #Material::paint_layers_*_delta field
 * each, written into one float socket of one of the two owned nodes.
 */
enum ePaintLayersResponseValue : int8_t {
  PAINT_LAYERS_RESPONSE_BUMP_STRENGTH = 0,
  PAINT_LAYERS_RESPONSE_BUMP_DISTANCE = 1,
  PAINT_LAYERS_RESPONSE_NORMAL_STRENGTH = 2,
};

/** What one response value is: where it lives and where it is written. */
struct PaintLayersResponseValueInfo {
  ePaintLayersResponseValue value;
  /** #RESPONSE_NODE_KEY_PROP of the node the value belongs to. */
  const char *node_key;
  /** The float input of that node the value is written into. */
  const char *socket;
  /** The highest value the field stores; the field keeps `value - 1`. */
  float maximum;
};

/** The three response values, in the order the panel offers them. */
Span<const PaintLayersResponseValueInfo> paint_layers_response_values();

/** The entry of \a value, or null for a value outside the enum. */
const PaintLayersResponseValueInfo *paint_layers_response_info(ePaintLayersResponseValue value);

/** The material field \a value is stored in, as the distance from one. */
float &response_field_delta(Material &ma, ePaintLayersResponseValue value);
/** The value the panel shows for \a value: `1 + delta`. */
float response_field(const Material &ma, ePaintLayersResponseValue value);
/**
 * Write the material's response values (#Material::paint_layers_bump_strength_delta and the other
 * two) into the owned nodes' Strength/Distance sockets, in place. A node without the marker is a
 * user's own and is left alone; tags nothing, so the evaluation can call it on an evaluated copy.
 */
void paint_layers_response_nodes_sync(Material &ma);
bNodeSocket *instance_output_find(bNodeTree &tree, bNode &instance, const char *result_name);
/** The socket the interface input named \a name builds on \a instance, or null. */
bNodeSocket *instance_input_find(bNodeTree &tree, bNode &instance, const char *name);
bNodeSocket *principled_channel_socket(Material &ma, int channel);
bNodeSocket *socket_in(bNode &node, const char *name);
bNodeSocket *socket_out(bNode &node, StringRefNull name);

/** The four floats a chain's bottom value is carried in, read off a node socket's default. The
 * wiring's adoption takes a graph's value this way (#source_input_default_set). */
void socket_value_read(const bNodeSocket &socket, float r_value[4]);
/** As #socket_value_read, into a node socket's default. */
void socket_value_write(bNodeSocket &socket, const float value[4]);

/** #bNode::prop key of the Vector Rotate and Mapping nodes a row's UV mapping chain is made of.
 * The marker, not the node name, tells a generated chain from a node of the source. */
inline constexpr const char *MAPPING_CHAIN_PROP = "pbr_paint_layers_mapping_chain";

/** Whether \a node is one of the nodes #mapping_chain_add made. */
bool node_is_mapping_chain(const bNode &node);

/** What of a shader tree reads the object's active UVs, one level only (groups are not entered). */
struct UvReaders {
  /** Texture Coordinate and unnamed UV Map nodes whose UV output feeds something. */
  Vector<bNode *> sources;
  /** Image Textures that feed the shader and have an open Vector input. */
  Vector<bNode *> open_images;

  bool any() const
  {
    return !sources.is_empty() || !open_images.is_empty();
  }
};

/**
 * The one rule for "reads the active UV": the predicate behind #BKE_paint_layers_source_reads_uv
 * and the source group's chain insertion both ask it, so what the UI offers is what gets moved.
 */
UvReaders uv_readers_find(bNodeTree &tree);

struct MappingChain {
  bNode *rotate = nullptr;
  bNode *mapping = nullptr;
  /**
   * The Vector Math nodes of the row's Mirror fold, empty when the row folds nothing. They sit
   * between the UV source and #rotate and read the fold center from \a center_socket.
   */
  Vector<bNode *> fold_nodes;
  /**
   * The fold's output: the node and socket holding the folded UV, which is what the transform
   * reads. Both null when the row folds nothing. Passed beside the socket for the same reason as
   * #side_sign_node.
   */
  bNode *fold_out_node = nullptr;
  bNodeSocket *fold_out = nullptr;
  /**
   * A Vector whose components are the fold's side signs at the canvas UV (+1 on and beyond the
   * center, -1 before it, +1 on an axis that does not fold), or null when the row folds nothing.
   * A tangent-space Normal multiplies by it before the rotation, the per-texel generalization of
   * the negative-scale flip. \a side_sign_node is the socket's owner, passed beside it because a
   * tree under construction has no topology cache to find it through.
   */
  bNodeSocket *side_sign = nullptr;
  bNode *side_sign_node = nullptr;
};

/**
 * Add the row transform in front of \a uv_out of \a uv_node: a Vector Rotate, then a `Point`
 * Mapping. Rotation, offset and scale are linked from \a values_node's sockets, and any of them
 * may be null (the node keeps its identity default). Neither node is placed. On failure nothing is
 * left in \a tree and the result is empty.
 *
 * When \a mirror_axes names folded axes and \a center_node / \a center_socket carry the fold
 * center, the chain grows the Mirror fold in front of the rotate -- the map is read through
 * `uv + mask * (|uv - center| + center - uv)` in the canvas UV, before the transform -- and
 * #MappingChain::fold_out carries the folded UV the rotate reads while #MappingChain::side_sign
 * carries the fold's side signs for a tangent-space Normal. The fold and the signs read the same
 * canvas UV the painting addresses, which is what makes the symmetry line up with the strokes. The
 * folded axes are the chain's shape; the center is a value. The owner node of the center socket is
 * passed beside it, because a tree under construction has no topology cache to find it through.
 */
MappingChain mapping_chain_add(bNodeTree &tree,
                               bNode &uv_node,
                               bNodeSocket &uv_out,
                               bNode *values_node,
                               bNodeSocket *offset,
                               bNodeSocket *scale,
                               bNodeSocket *rotation,
                               bNode *center_node = nullptr,
                               bNodeSocket *center_socket = nullptr,
                               int16_t mirror_axes = 0);

/**
 * Add the covariant remap of an encoded tangent-space normal read from \a color_socket: decode,
 * flip by the sign of the scale, rotate back, encode. \a rotation and \a scale are the sockets
 * that drive the row transform (null scale is the identity). \a side_sign / \a side_sign_node,
 * when non-null, are the row's Mirror fold side-sign vector and its owner: the decoded normal
 * multiplies by it before the scale flip, which turns the component along each folded axis per
 * texel, on the side of the fold center the texel reads from. The nodes made are appended to
 * \a r_made, unplaced. \return the encoded output, or null with nothing left in \a tree.
 */
bNodeSocket *normal_remap_nodes_add(bNodeTree &tree,
                                    bNode &color_node,
                                    bNodeSocket &color_socket,
                                    bNode *rotation_node,
                                    bNodeSocket *rotation,
                                    bNode *scale_node,
                                    bNodeSocket *scale,
                                    bNode *side_sign_node,
                                    bNodeSocket *side_sign,
                                    Vector<bNode *> &r_made,
                                    bNode *&r_encode);

/**
 * Add the frame-to-tangent re-expression of a projected row's encoded tangent-space normal read
 * from \a color_socket: decode, rotate through the projection frame's own basis into the surface's
 * space, encode in the surface tangent basis (the tangent the active render UV map gives at the
 * shaded point, the bitangent as its cross with the surface normal), and renormalize. The one
 * conversion #BKE_paint_layers_projection_normal_frame_to_tangent runs on the CPU bake, built as
 * the matching node chain, so the viewport and the bake shade the same relief. \a rotation is the
 * frame's orientation value (the row's projection rotation socket; null is the identity). The
 * nodes made are appended to \a r_made, unplaced. \return the encoded output, or null with
 * nothing left in \a tree.
 */
bNodeSocket *normal_frame_tangent_nodes_add(bNodeTree &tree,
                                            bNode &color_node,
                                            bNodeSocket &color_socket,
                                            bNode *rotation_node,
                                            bNodeSocket *rotation,
                                            Vector<bNode *> &r_made,
                                            bNode *&r_encode);

/** The same without a Mirror side-sign vector: the old signature, for callers and tests that do
 * not fold anything. */
inline bNodeSocket *normal_remap_nodes_add(bNodeTree &tree,
                                           bNode &color_node,
                                           bNodeSocket &color_socket,
                                           bNode *rotation_node,
                                           bNodeSocket *rotation,
                                           bNode *scale_node,
                                           bNodeSocket *scale,
                                           Vector<bNode *> &r_made,
                                           bNode *&r_encode)
{
  return normal_remap_nodes_add(tree,
                                color_node,
                                color_socket,
                                rotation_node,
                                rotation,
                                scale_node,
                                scale,
                                nullptr,
                                nullptr,
                                r_made,
                                r_encode);
}

/**
 * Link the row-mapping input of \a tree's Group Input that \a from_iface stands for into the input
 * of \a instance (a group node whose tree carries the same \a role, see
 * #source_group_mapping_input_find). Idempotent: an already linked input is left alone, and a
 * missing side leaves the instance at its identity default.
 */
void mapping_input_relay(bNodeTree &tree,
                         bNode &group_input,
                         const bNodeTreeInterfaceSocket *from_iface,
                         bNode &instance,
                         const char *role);

IDProperty *properties_ensure(IDProperty *&properties);
void prop_int_set(IDProperty *&properties, const char *key, int value);
void prop_string_set(IDProperty *&properties, const char *key, const char *value);

/**
 * Mark a ColorRamp node with the row and channel it shows (#RAMP_NODE_KEY_PROP, `R:<marker>:<channel>`,
 * -1 for the row's shared colour ramp), so an edit of stops or colors is copied into its storage in
 * place (#paint_layers_gradient_ramp_sync) instead of rebuilding the group.
 */
void ramp_node_key_set(bNode &ramp, const MaterialPaintLayer &row, int channel);
int prop_int_get(const IDProperty *properties, const char *key, int fallback);
const char *prop_string_get(const IDProperty *properties, const char *key);
void uid_prop_set(IDProperty *&properties, const char *key, const bUUID &uid);
bool uid_prop_get(const IDProperty *properties, const char *key, bUUID &r_uid);

bUUID tree_owner_uid_get(const bNodeTree &tree);
void tree_owner_uid_set(bNodeTree &tree, const bUUID &uid);
void instance_uid_set(bNode &node, const bUUID &uid);
bool layer_tree_marker_get(const bNodeTree &tree, bUUID &r_marker);

/* -------------------------------------------------------------------- */
/** \name Topology hashes
 *
 * A row's group and the root are a function of the description's structure alone; their values
 * (opacity, a Fill constant, the enabled flag) are inputs and never move these hashes. When a hash
 * matches the one stored on the tree, the tree is kept as it is and only its values are re-synced.
 * \{ */

/** Order-sensitive mix into the FNV-1a style accumulator. */
uint64_t topology_hash_mix(uint64_t hash, uint64_t value);
void topology_hash_string(uint64_t &hash, const char *text);
void topology_hash_uid(uint64_t &hash, const bUUID &uid);

/** Store/read a 64-bit hash as two 32-bit IDProperty words (IDProperty has no 64-bit int). */
void tree_hash_set(bNodeTree &tree, const char *low_key, const char *high_key, uint64_t hash);
bool tree_hash_get(bNodeTree &tree, const char *low_key, const char *high_key, uint64_t &r_hash);
/** Drop every stored topology hash of \a tree, so it reads as "changed" until a build sets one. */
void tree_hash_clear(bNodeTree &tree);
void tree_topology_hash_set(bNodeTree &tree, uint64_t hash);
bool tree_topology_hash_get(bNodeTree &tree, uint64_t &r_hash);
void tree_root_hash_set(bNodeTree &tree, uint64_t hash);
bool tree_root_hash_get(bNodeTree &tree, uint64_t &r_hash);

/**
 * What a row's place in the stack decides about how it is built: whether an isolating folder lies
 * above it (the row then works on the folder's pre-multiplied accumulation, in a group of its own
 * shape), and the factor the Pass Through folders above it put on its opacity (zero when one of
 * them is hidden).
 */
struct PaintLayerForm {
  bool in_isolating_folder = false;
  float pass_through_scale = 1.0f;
};

/** The form of every row of \a ma, found by one walk instead of one search per row. */
using PaintLayerFormMap = Map<const MaterialPaintLayer *, PaintLayerForm>;

/**
 * Walk \a ma's stack once and record the #PaintLayerForm of every row, at any depth. The one place
 * the rule lives, so the topology hash, the builder and the value sync cannot disagree on it.
 */
PaintLayerFormMap paint_layers_form_map(const Material &ma);

/**
 * How many times a build tore down a kept group because its instance could not be re-linked (see
 * #PaintLayersTreeBuilder::row_rebuild_kept). The topology hash is what has to notice a structural
 * edit, before the build and without touching the group; a rebuild is only the net under it, so a
 * healthy pass leaves this at zero. The tests of the hash read it to prove an edit was caught.
 */
int paint_layers_kept_group_rebuild_count();
void paint_layers_kept_group_rebuild_count_reset();

/**
 * The structure of one row: its source, blend, name and per-channel participation. Never a value.
 * \a forms is #paint_layers_form_map of \a ma, built once by the caller for all its rows.
 */
uint64_t paint_layers_layer_topology_hash(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          Span<int> wired_channels,
                                          const PaintLayerFormMap &forms);

/**
 * The structure of the root: the wired channels, and the participating rows in order with their
 * per-channel participation. A reorder or a structural edit moves it; a value edit does not.
 */
uint64_t paint_layers_root_topology_hash(const Material &ma, Span<int> wired_channels);

/**
 * Bring the `blend_type` of every generated blend node in line with the description, in place.
 * \return false when the tree no longer carries the nodes the description expects (a hand-edited
 * tree, or one built before the nodes were keyed), so the caller falls back to a full rebuild.
 */
bool paint_layers_blend_sync(Material &ma, Main &bmain);

/**
 * Bring the image pointer of every keyed channel map node in line with the description, in place.
 * A swap from one image to another is the same node, so the group keeps its shape; the bake hash
 * still moves (it keys the image), so a substituted row re-bakes.
 * \return false when the tree no longer carries the nodes the description expects, so the caller
 * falls back to a full rebuild.
 */
bool paint_layers_maps_sync(Material &ma, Main &bmain);

/**
 * Copy each keyed ColorRamp's band from the description into its node storage, in place, for the
 * ramps whose shader shape (one stop / two-stop uniforms / band texture) did not change. The ramp
 * pixels are uniforms or texture content in the shader, so this compiles nothing.
 * \return false when a ramp's shape changed or its node is missing, so the caller rebuilds.
 */
bool paint_layers_gradient_ramp_sync(Material &ma, Main &bmain);

/** Record how many keyed blend nodes each layer group holds, after a build. */
void paint_layers_blend_nodes_commit(Material &ma);

/**
 * Bring every generated RGB Curves node's storage in line with its row's curve mapping, in place —
 * the same deal the blend sync offers a `blend_type`: a point edit re-copies the mapping into the
 * node and rebuilds no group. \return false when the tree no longer carries the curves nodes the
 * description expects (a hand-edited tree, or one built before the nodes were keyed), so the
 * caller falls back to a full rebuild.
 */
bool paint_layers_curves_sync(Material &ma, Main &bmain);

/** Record how many keyed curves nodes each layer group holds, after a build. */
void paint_layers_curves_nodes_commit(Material &ma);

/** \} */

/**
 * Whether \a layer takes part in \a channel: it carries a live record that has a map, or a flat value
 * with some coverage, or a content correction lays a map there. One predicate for the build and the
 * CPU composite
 * (#BKE_paint_layers_composite_image_layers), so a row cannot take part on one side and drop out on
 * the other.
 */
bool leaf_participates(const Material &ma, const MaterialPaintLayer &layer, int channel);

/**
 * Whether \a layer takes part in \a channel as a row of the chain: a leaf through its own record
 * (#leaf_participates), a folder through any row it holds. A folder owns no channel records, so its
 * participation is the union of its children's; an emptied folder takes part in nothing.
 */
bool row_participates(const Material &ma, const MaterialPaintLayer &layer, int channel);

/**
 * The channels \a ma wires at all, in channel order: a channel nothing participates in gets no
 * output, so the material keeps whatever the user had on that Principled input.
 */
Vector<int> paint_layers_wired_channels(const Material &ma);

/** A unique interface name for \a base, so a renamed or duplicated layer never collides. */
void interface_name_unique(const bNodeTreeInterface &interface,
                           const char *base,
                           char *r_buffer,
                           size_t buffer_size);

/** The marker of the row \a tree is the source group of; false for every other tree. */
bool source_group_row_get(const bNodeTree &tree, bUUID &r_marker);

/** Mark \a tree as the source group of the row \a marker. */
void source_group_row_set(bNodeTree &tree, const bUUID &marker);

/**
 * Fill the empty or stale group \a wrapper with a copy of \a source's shader graph: every node but
 * the Material Output and the Principled BSDF, with the links among them, and one output per paint
 * channel the Principled input of which is fed by a node (the Normal as the encoded map the chain
 * reads), plus the coverage from Alpha. The nodes are cleared first and the interface is kept, so
 * the instances stay linked. When the Principled BSDF
 * sits inside groups of the source, each group on the way is copied privately
 * (#TREE_SOURCE_PATH_PROP) and exposes the channels one level up; the groups beside the path stay
 * shared with the source (#BKE_paint_layers_source_group_wrappable).
 *
 * \return false when the source cannot be wrapped; the group is then left empty.
 */
bool source_group_fill(Main &bmain,
                       bNodeTree &wrapper,
                       const Material &source,
                       bool mapped = false,
                       int16_t mirror_axes = 0,
                       Span<const MaterialPaintLayer *> transform_effects = {});

/** Delete the private group copies of \a owner_uid that no instance node uses any more. */
void source_group_paths_prune(Main &bmain, const bUUID &owner_uid);

/** The output of \a instance (an instance of the source group \a wrapper) for the paint channel.
 */
bNodeSocket *source_group_output(const bNodeTree &wrapper, bNode &instance, int channel);

/**
 * Take the socket values of \a source into the group \a wrapper filled from it, in place: the
 * nodes of the copy and of the private group copies below it are found by name in the source and
 * get its default values. Valid only while the source's structure hash is the one the group was
 * filled from. \return false when a node or socket has no counterpart: the group must be refilled.
 */
bool source_group_values_sync(Main &bmain, bNodeTree &wrapper, const Material &source);

/** The structure hash \a wrapper was filled from (see #TREE_SOURCE_STRUCTURE_LOW_PROP). */
void source_group_structure_hash_set(bNodeTree &wrapper, uint64_t hash);
bool source_group_structure_hash_get(bNodeTree &wrapper, uint64_t &r_hash);

/**
 * Whether the group of \a source exposes a Normal: only a Normal Map node feeds one, any other
 * normal (a Bump) comes from screen-space derivatives and is read from the rendered map instead.
 */
bool source_group_normal_live(const Material &source);

/** How a row maps its source group: whether the wrapper carries Mapping nodes, and which axes fold. */
struct SourceGroupMapping {
  bool mapped = false;
  int16_t axes = 0;
  /** The row's transform effects whose mapping remaps, in the effects list's order: the chains
   * the wrapper inserts ahead of the row's own, and the per-effect inputs it carries. */
  Vector<const MaterialPaintLayer *> effects;
};

/** The mapping of the source group of layer; the axes only count while the row remaps, and the
 * transform effects ride along: their chains are part of the same wrapper. */
inline SourceGroupMapping source_group_mapping_get(const Material &ma,
                                                   const MaterialPaintLayer &layer)
{
  SourceGroupMapping mapping;
  mapping.mapped = BKE_paint_layers_mapping_remaps(ma, layer);
  mapping.axes = mapping.mapped ? BKE_paint_layers_mapping_mirror_get(layer) : 0;
  for (const MaterialPaintLayer *effect : BKE_paint_layers_active_uv_transform_effects(ma, layer)) {
    mapping.effects.append(effect);
    mapping.mapped = true;
  }
  return mapping;
}

/** The hash the source group stores in its topology words: the source's content and the fill's
 * version. \a mapped is part of it because a mapped wrapper carries Mapping nodes and their three
 * interface inputs, so toggling the row's mapping has to refill the group once; \a mirror_axes is
 * part of it the same way, because a folding wrapper carries the fold's nodes; \a effects is part
 * of it because their chains and their own inputs are, and a fold's axes are chain shape. */
uint64_t source_group_content_hash(const Material &source,
                                   bool mapped = false,
                                   int16_t mirror_axes = 0,
                                   Span<const MaterialPaintLayer *> effects = {});

/** The wrapper interface input whose #INPUT_ROLE_PROP is \a role, whatever its name. */
bNodeTreeInterfaceSocket *source_group_mapping_input_find(bNodeTree &tree, const char *role);

/** The wrapper input role of one transform effect's mapping values: \a base with the effect's
 * marker appended, so an effect's inputs never collide with the row's own. The same words both
 * sides of the relay speak. */
std::string source_group_effect_mapping_role(const char *base, const MaterialPaintLayer &effect);

/** Whether \a tree actually carries a generated row-mapping chain (a source with no UV reader
 * gets the interface inputs but no chain, and must not be treated as mapped). */
bool source_group_mapping_present(const bNodeTree &tree);

/* The mapping half of the fill (`paint_layers_generate_source_group_mapping.cc`). */

/** The Group Input of \a tree, created when the copied tree has none. */
bNode *source_group_group_input_ensure(bNodeTree &tree);

/** The Group Input socket of \a tree's mapping input named by \a role, or null. */
bNodeSocket *source_group_mapping_input_socket(bNodeTree &tree,
                                               bNode &group_input,
                                               const char *role);

/**
 * The Mirror fold's side-sign output of \a tree's mapping chain, or null when the level folds
 * nothing. \a r_node, when asked, takes the socket's owner node (a tree under construction has no
 * topology cache to find it through).
 */
bNodeSocket *source_group_side_sign_find(bNodeTree &tree, bNode **r_node);

/**
 * Make \a tree one mapped level: the value inputs, the implicit UV wiring and the chain in front
 * of every UV reader. \a mirror_axes names the axes the row's Mirror folds, which decides whether
 * the chain grows the fold's nodes beside the transform. \return whether a chain was inserted, so
 * a level without a UV reader is not treated as mapped.
 */
/**
 * The mapping half of a mapped level: inputs (the row's own and one set per transform effect),
 * the implicit UV wiring, the effects' chains and the row's chain in front of every UV reader.
 * \a effects are the row's transform effects whose mapping remaps, in list order. Returns whether
 * a chain was actually inserted.
 */
bool source_group_mapping_prepare(bNodeTree &tree,
                                  int16_t mirror_axes = 0,
                                  Span<const MaterialPaintLayer *> effects = {});

/** Drop the value inputs again once the row applies no mapping (the root keeps its interface). */
void source_group_mapping_interface_remove(bNodeTree &tree);

/** Relay \a tree's mapping inputs into the child path \a instance. */
void source_group_mapping_instance_wire(bNodeTree &tree, bNode &group_input, bNode &instance);

/** Whether the colour fed by \a node of \a tree follows the UV the row mapping moved. */
bool source_group_color_reads_mapped_uv(const bNodeTree &tree, const bNode &node);

/** The same without the socket values: what a group filled from the source keeps while they move.
 * \a mapped is part of it too: a mapped wrapper carries Mapping nodes, so a toggle must not be
 * mistaken for a value-only edit by the refresh's in-place sync; \a mirror_axes for the same
 * reason, and \a effects because their chains and their own inputs are. */
uint64_t source_group_structure_hash(const Material &source,
                                     bool mapped = false,
                                     int16_t mirror_axes = 0,
                                     Span<const MaterialPaintLayer *> effects = {});

/** Remove every node of \a tree, leaving its interface untouched. */
void tree_clear_nodes(Main &bmain, bNodeTree &tree);
/** Remove every node and every interface item of \a tree. */
void tree_clear(Main &bmain, bNodeTree &tree);

/** Copy the layer groups of a ripped tree, re-pointing the nodes at the copies. */
void layer_trees_copy(Main *bmain,
                      bNodeTree &tree,
                      const bUUID &src_owner,
                      const bUUID &dst_owner);

/** Ensure a Principled exists and drives the Surface input of the material. */
void principled_ensure(Material &ma, PaintLayersRegenerateReport &r_report);

/**
 * Link \a from_socket into \a input, replacing whatever was there. The generator owns the inputs
 * it routes into, so a foreign link is restored over and reported.
 */
void input_link_restore(Material &ma,
                        bNodeSocket &input,
                        bNode &from_node,
                        bNodeSocket &from_socket,
                        PaintLayersRegenerateReport &r_report);

/** Route every generated output of \a instance into the material, channel by channel. */
void wire_instance_to_material(Material &ma,
                               bNodeTree &tree,
                               bNode &instance,
                               PaintLayersRegenerateReport &r_report);

/* -------------------------------------------------------------------- */
/** \name Row map holders
 *
 * What images a stack row's description holds. The graph self-heal and the paint target both ask
 * it -- the heal to decide whether a lost image node is a defect, the target to decide whether a
 * map is foreign -- so the one definition lives here rather than in either of them.
 * \{ */

/**
 * The images a row legitimately reads in the generated graph: its own channel records' maps, the
 * maps of its corrections and mask items, and its bake's snapshot maps. A bake-built row reads the
 * bake's copies, not the live maps, so both sets are allowed.
 */
Vector<const Image *> row_map_images(const MaterialPaintLayer &row);

/**
 * Whether the stack still holds at least one map for \a row: its own records, its corrections and
 * mask items, or a mesh-map slot of \a ma.
 */
bool row_has_maps(const Material &ma, const MaterialPaintLayer &row);

/** \} */

/**
 * One link from the author's graph into the generated instance's `Source <channel>` input of a
 * source-graph material.
 *
 * A full rebuild clears the root's interface and re-creates its sockets, which mints fresh
 * identifiers and takes the instance's sockets -- and every link into them -- with the old ones.
 * The interface socket's name is the one thing a rebuild keeps, so it is what a link is remembered
 * by across one (#source_links_capture, #source_links_restore). The author's node and socket are
 * remembered by identifier rather than by pointer: the capture and the restore run either side of
 * a rebuild, and nothing between them may be allowed to matter for the pointers' validity.
 */
struct PaintLayersSourceLink {
  /** #bNode::identifier of the author's node the link came from. */
  int from_node_id = 0;
  /** #bNodeSocket::identifier of the author's output the link came from. */
  std::string from_socket_identifier;
  /** The name of the interface input the link fed; owned copy, valid after the rebuild. */
  std::string to_name;
};

/**
 * Write a source-graph channel's bottom value to the interface socket, the root's Group Input and
 * the instance socket, keeping each socket's own type. The three are the same value at different
 * stages of the generated tree's life, so they must be written together.
 */
void source_input_value_set(bNodeTree &tree,
                            bNodeTreeInterfaceSocket *iface,
                            const float value[4],
                            bNodeSocket *instance_in);

/** The graph's links into \a ma's generated instance, taken before a root rebuild clears them. */
Vector<PaintLayersSourceLink> source_links_capture(const Material &ma);

/** Re-aim the captured links at the instance's sockets of the rebuilt root. A link whose author
 * node no longer exists, or whose re-aim would form a cycle, is dropped. */
void source_links_restore(Material &ma, bNode &instance, Span<PaintLayersSourceLink> links);

}  // namespace bke::paint_layers
}  // namespace blender
