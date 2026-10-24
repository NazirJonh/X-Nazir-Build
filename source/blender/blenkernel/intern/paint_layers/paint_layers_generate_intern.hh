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

#include "BKE_paint_layers_generate.hh"

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_uuid_types.h"

namespace blender {

struct bNode;
struct bNodeSocket;
struct bNodeTree;
struct bNodeTreeInterface;
struct bNodeTreeInterfaceSocket;
struct IDProperty;
struct Main;
struct Material;
struct MaterialPaintLayer;

namespace bke::paint_layers {

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

/** On a row's opacity input: set (to 1) when the row was built from its bake, so that input holds
 * the row's visibility alone -- the bake already holds its opacity -- and the value sync can tell a
 * group built that way from one built live. */
inline constexpr const char *INPUT_SUBSTITUTED_PROP = "pbr_paint_layers_baked";

/** #INPUT_ROLE_PROP value of a layer's opacity (enabled already folded in). A content correction's
 * opacity input carries the same role with the correction's own marker. */
inline constexpr const char *ROLE_OPACITY = "opacity";
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
/** #INPUT_ROLE_PROP value of a row's UV mapping offset (a Vector input holding offset, 0). The
 * marker is the row's own, whether a layer, a correction or a mask item. */
inline constexpr const char *ROLE_MAPPING_OFFSET = "mapping_offset";
/** #INPUT_ROLE_PROP value of a row's UV mapping scale (a Vector input holding scale, 1). */
inline constexpr const char *ROLE_MAPPING_SCALE = "mapping_scale";
/** #INPUT_ROLE_PROP value of a row's UV mapping rotation (a Vector input holding 0, 0, angle). */
inline constexpr const char *ROLE_MAPPING_ROTATION = "mapping_rotation";
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
/**
 * On a source-group wrapper's interface input: the row mapping value relayed into the wrapper.
 * The same three roles travel from the row group into the wrapper instance and down every private
 * group copy on the path to the source's Principled BSDF, so the Mapping nodes built inside the
 * wrapper read the row's values without a rebuild when a slider moves.
 */
inline constexpr const char *SOURCE_GROUP_ROLE_MAPPING_OFFSET = "source_mapping_offset";
inline constexpr const char *SOURCE_GROUP_ROLE_MAPPING_SCALE = "source_mapping_scale";
inline constexpr const char *SOURCE_GROUP_ROLE_MAPPING_ROTATION = "source_mapping_rotation";
/** Prefix of the wrapper's three row-mapping value inputs. */
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
bNode *normal_map_ensure(Material &ma);
bNode *bump_ensure(Material &ma);
bNodeSocket *instance_output_find(bNodeTree &tree, bNode &instance, const char *result_name);
bNodeSocket *principled_channel_socket(Material &ma, int channel);
bNodeSocket *socket_in(bNode &node, const char *name);
bNodeSocket *socket_out(bNode &node, StringRefNull name);

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
};

/**
 * Add the row transform in front of \a uv_out of \a uv_node: a Vector Rotate, then a `Point`
 * Mapping. Rotation, offset and scale are linked from \a values_node's sockets, and any of them
 * may be null (the node keeps its identity default). Neither node is placed. On failure nothing is
 * left in \a tree and the result is empty.
 */
MappingChain mapping_chain_add(bNodeTree &tree,
                               bNode &uv_node,
                               bNodeSocket &uv_out,
                               bNode *values_node,
                               bNodeSocket *offset,
                               bNodeSocket *scale,
                               bNodeSocket *rotation);

/**
 * Add the covariant remap of an encoded tangent-space normal read from \a color_socket: decode,
 * flip by the sign of the scale, rotate back, encode. \a rotation and \a scale are the sockets
 * that drive the row transform (null scale is the identity). The nodes made are appended to
 * \a r_made, unplaced. \return the encoded output, or null with nothing left in \a tree.
 */
bNodeSocket *normal_remap_nodes_add(bNodeTree &tree,
                                    bNode &color_node,
                                    bNodeSocket &color_socket,
                                    bNode *rotation_node,
                                    bNodeSocket *rotation,
                                    bNode *scale_node,
                                    bNodeSocket *scale,
                                    Vector<bNode *> &r_made,
                                    bNode *&r_encode);

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

/** Record how many keyed blend nodes each layer group holds, after a build. */
void paint_layers_blend_nodes_commit(Material &ma);

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
                       bool mapped = false);

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

/** The hash the source group stores in its topology words: the source's content and the fill's
 * version. \a mapped is part of it because a mapped wrapper carries Mapping nodes and their three
 * interface inputs, so toggling the row's mapping has to refill the group once. */
uint64_t source_group_content_hash(const Material &source, bool mapped = false);

/** The wrapper interface input whose #INPUT_ROLE_PROP is \a role, whatever its name. */
bNodeTreeInterfaceSocket *source_group_mapping_input_find(bNodeTree &tree, const char *role);

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
 * Make \a tree one mapped level: the value inputs, the implicit UV wiring and the chain in front
 * of every UV reader. \return whether a chain was inserted, so a level without a UV reader is not
 * treated as mapped.
 */
bool source_group_mapping_prepare(bNodeTree &tree);

/** Drop the value inputs again once the row applies no mapping (the root keeps its interface). */
void source_group_mapping_interface_remove(bNodeTree &tree);

/** Relay \a tree's mapping inputs into the child path \a instance. */
void source_group_mapping_instance_wire(bNodeTree &tree, bNode &group_input, bNode &instance);

/** Whether the colour fed by \a node of \a tree follows the UV the row mapping moved. */
bool source_group_color_reads_mapped_uv(const bNodeTree &tree, const bNode &node);

/** The same without the socket values: what a group filled from the source keeps while they move.
 * \a mapped is part of it too: a mapped wrapper carries Mapping nodes, so a toggle must not be
 * mistaken for a value-only edit by the refresh's in-place sync. */
uint64_t source_group_structure_hash(const Material &source, bool mapped = false);

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

}  // namespace bke::paint_layers
}  // namespace blender
