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
 */

#include <cstddef>

#include "BKE_paint_layers_generate.hh"

#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_uuid_types.h"

namespace blender {

struct bNode;
struct bNodeSocket;
struct bNodeTree;
struct bNodeTreeInterface;
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
/** On a layer's own node group: the marker of the layer it holds. The tree also carries
 * #TREE_OWNER_PROP of the material, so a copy never adopts the source's groups. */
inline constexpr const char *TREE_LAYER_PROP = "pbr_paint_layers_layer_tree";
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

/** #INPUT_ROLE_PROP value of a layer's opacity (enabled already folded in). */
inline constexpr const char *ROLE_OPACITY = "opacity";
/** #INPUT_ROLE_PROP value of the constant of a channel that carries no map. */
inline constexpr const char *ROLE_FILL = "fill";

bNode *instance_find(const Material &ma, const bUUID &owner_uid);
bNode *material_output_find(Material &ma);
bNode *mix_node_add(bNodeTree &tree, int ramp_blend, float location_x, float location_y);
bNode *normal_map_ensure(Material &ma);
bNode *bump_ensure(Material &ma);
bNodeSocket *instance_output_find(bNodeTree &tree, bNode &instance, const char *result_name);
bNodeSocket *principled_channel_socket(Material &ma, int channel);
bNodeSocket *socket_in(bNode &node, const char *name);
bNodeSocket *socket_out(bNode &node, StringRefNull name);

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
void tree_topology_hash_set(bNodeTree &tree, uint64_t hash);
bool tree_topology_hash_get(bNodeTree &tree, uint64_t &r_hash);
void tree_root_hash_set(bNodeTree &tree, uint64_t hash);
bool tree_root_hash_get(bNodeTree &tree, uint64_t &r_hash);

/**
 * The structure of one row: its source, blend, name and per-channel participation. Never a value.
 */
uint64_t paint_layers_layer_topology_hash(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          Span<int> wired_channels);

/**
 * The structure of the root: the wired channels, and the participating rows in order with their
 * per-channel participation. A reorder or a structural edit moves it; a value edit does not.
 */
uint64_t paint_layers_root_topology_hash(const Material &ma, Span<int> wired_channels);

/** \} */

/**
 * Whether \a layer takes part in \a channel: it carries a live record that has a map, or a flat value
 * with some coverage. One predicate for the build and the CPU composite
 * (#BKE_paint_layers_composite_image_layers), so a row cannot take part on one side and drop out on
 * the other.
 */
bool leaf_participates(const MaterialPaintLayer &layer, int channel);

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
