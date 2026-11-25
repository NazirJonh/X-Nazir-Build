/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The node-group kinds of the paint-layer stack: a row that reads the user's own shader node tree
 * (`#MaterialPaintLayer::kind_tree`) instead of answering from a registry schema.
 *
 * Three kinds read a tree: the #MA_PAINT_LAYER_SOURCE_NODE_GROUP layer lays the group's paint
 * channels as its content, the #MA_PAINT_LAYER_FILTER_NODE_GROUP effect lays the group's `Result`
 * over what it reads, and the #MA_PAINT_LAYER_GENERATOR_NODE_GROUP generator indexes its ramp with
 * the group's `Fac`. What the group must expose is a convention, spelled out by the socket names
 * here; what every other input means is a parameter of the row's dynamic kind schema, so the same
 * tree serves any number of rows with their own values.
 *
 * This header is the vocabulary the convention is written in: the reserved socket names, the
 * validation a tree must pass to serve as one kind or another, and the interface hash the caches
 * (the dynamic schema, the shader-bake) key on. The parameter extraction itself lives with the
 * dynamic kind registry (`paint_layers_kind_dynamic.cc`), and the evaluation lives with the bake
 * and the build -- nothing here walks a parameter, so the conventions can be checked and hashed
 * without the stack around.
 */

#include <cstdint>

#include "BLI_span.hh"
#include "BLI_string_ref.hh"

namespace blender {

struct bNodeTree;
struct Main;
struct Material;
struct MaterialPaintLayer;

/* -------------------------------------------------------------------- */
/** \name The interface convention
 *
 * The names below are the sockets the kinds read and write by name. An input that is not one of
 * the reserved ones is a parameter; an output that is neither #PAINT_NG_SOCKET_RESULT nor a paint
 * channel's name is ignored (the validator reports it).
 * \{ */

/** The content a node-group filter works on: the channel's colour, or the mask's grey as a mask
 * item. Required for a filter tree. */
constexpr const char *PAINT_NG_SOCKET_INPUT = "Input";
/** The UV (or the row's projected coordinate) the layer reads its content at. Optional: without
 * it the group reads the texture coordinates itself, and the row's projection frame does not
 * reach it. */
constexpr const char *PAINT_NG_SOCKET_UV = "UV";
/**
 * The coverage the row lays under, as a layer's input; the grey the mask has built up to the item,
 * as a mask item's. Optional: a filter without it is pointwise over the input alone, and a layer
 * without it lays everywhere its output covers.
 *
 * NOTE: Reserved for v2. The shader-bake backend does not bind this input yet (there is no
 * coverage source in #PaintLayerShaderBakeRequest); a tree that wires it reads the socket
 * default. Do not add it to a template until the backend feeds it.
 */
constexpr const char *PAINT_NG_SOCKET_MASK = "Mask";
/** The answer of a filter or a layer tree: the colour the row lays. Required for both. */
constexpr const char *PAINT_NG_SOCKET_RESULT = "Result";
/**
 * The alpha the result covers by, as a filter tree's second answer beside #PAINT_NG_SOCKET_RESULT.
 * The kind reads it: a filter tree that exposes one lays a layer with an alpha of its own (the
 * Drop Shadow's contract -- premultiplied colour and the alpha it covers by), and the descriptor
 * carries #PAINT_LAYER_KIND_RESULT_HAS_ALPHA so the composite raises the row's coverage by it. A
 * source tree exposing it answers nothing by it: the source's coverage is its map's own alpha
 * already, so the output is simply not wired. A generator has no alpha to lay.
 */
constexpr const char *PAINT_NG_SOCKET_ALPHA = "Alpha";
/** The placed coordinate the placement chain feeds a generator tree. Required for one. */
constexpr const char *PAINT_NG_SOCKET_COORDINATE = "Coordinate";
/** The pattern value a generator tree answers: the position the ramp indexes. Required for one. */
constexpr const char *PAINT_NG_SOCKET_FAC = "Fac";
/** The colour a generator tree lays where no ramp could express the answer (the Voronoi cell
 * colour). Optional. */
constexpr const char *PAINT_NG_SOCKET_COLOR = "Color";

/** The identifier of the group node instance the build lays for a row's tree, and the name of the
 * tree a new row's template is created with, per kind. */
constexpr const char *PAINT_NG_FILTER_TREE_PREFIX = ".PL NG Filter ";
constexpr const char *PAINT_NG_SOURCE_TREE_PREFIX = ".PL NG Layer ";
constexpr const char *PAINT_NG_GENERATOR_TREE_PREFIX = ".PL NG Generator ";

/** \} */

/* -------------------------------------------------------------------- */
/** \name Validation
 * \{ */

/**
 * What a tree exposes, as far as the kinds and the validator care. Read once per tree per
 * inspection; the caches derive their keys from it.
 */
struct PaintLayerNodeGroupShape {
  /** The tree has the sockets the kind needs. */
  bool has_input = false;
  bool has_result = false;
  bool has_uv = false;
  bool has_mask = false;
  bool has_coordinate = false;
  bool has_fac = false;
  bool has_color = false;
  /**
   * The tree has an `Alpha` output: the alpha a filter tree's result covers by. Only a filter
   * reads it -- a source tree's coverage is its map's own alpha already, so its `Alpha` output is
   * a no-op the validator does not even report.
   */
  bool has_alpha = false;
  /** One output names a paint channel (the layer kind routes it there). */
  bool has_channel_output = false;
  /**
   * The graph reaches for a neighborhood answer (a Bevel or an Ambient Occlusion node): the
   * composite cannot treat the kind as pointwise, so the reach is WholeMap and partial updates
   * keep every region. The tree is still legal -- the flag only prices it.
   */
  bool neighborhood_nodes = false;
  /**
   * The graph answers from the render (an Object Info, a Camera or a Light Path node): the result
   * depends on what object the material is evaluated on. Legal like the source graph's own
   * object-dependence; reported so the user knows the map is per object.
   */
  bool object_dependent = false;
  /** Count of inputs that would be parameters but expose nothing this build can answer (menus,
   * geometry sockets). A count, not the names: no message names the sockets, so none are stored.
   * Reserved -- the interface scan does not fill it yet. */
  int unsupported_params = 0;
};

/** Read the shape of \a tree's interface and graph. A null tree answers a default (empty) shape. */
PaintLayerNodeGroupShape BKE_paint_layers_node_group_shape(const bNodeTree *tree);

/**
 * Whether \a tree can serve as a #MA_PAINT_LAYER_FILTER_NODE_GROUP (a `Result` and an `Input`),
 * as a #MA_PAINT_LAYER_SOURCE_NODE_GROUP layer (a `Result`, or one output named after a paint
 * channel), or as a #MA_PAINT_LAYER_GENERATOR_NODE_GROUP (`Fac` and a `Coordinate`). A null tree
 * or a non-shader tree answers false.
 */
bool BKE_paint_layers_node_group_supports_filter(const bNodeTree *tree);
bool BKE_paint_layers_node_group_supports_source(const bNodeTree *tree);
bool BKE_paint_layers_node_group_supports_generator(const bNodeTree *tree);

/** Whether \a row's tree reaches for a neighborhood answer, so its reach is WholeMap. */
bool BKE_paint_layers_node_group_reaches_whole_map(const MaterialPaintLayer &row);

/** \} */

/* -------------------------------------------------------------------- */
/** \name The template trees
 *
 * Every node-group kind starts from a template: a shader node tree the build (or the user, in the
 * Shader Editor) fills in, with the convention's sockets already in place and the Group
 * Input/Output nodes to wire through. The templates live here, beside the validator they must
 * always satisfy, so the Add operator and the kind switch hand out the same shapes.
 * \{ */

/**
 * Which of the node-group kinds a template is made for: the code the Add operator's menu passes.
 * The order mirrors the operator's own picker.
 */
enum class PaintLayerNodeGroupTemplateKind {
  /** A filter tree: `Input` in, `Result` out, `UV` and `Strength` beside them. */
  Filter = 0,
  /** A layer tree: `UV` in, one paint channel's name (`Base Color`) out. A single-answer tree
   * renames that output to `Result`; the template authors the per-channel shape. */
  SourceLayer = 1,
  /** A generator tree: `Coordinate` in, `Fac` and `Color` out, a `Detail` parameter. */
  Generator = 2,
};

/**
 * Build a fresh template tree for \a kind: a standalone shader node tree in \a bmain carrying the
 * convention's sockets for the kind (in the order the row's dynamic schema reads them in), the
 * Group Input/Output nodes the user starts from, and the parameter defaults the Add flows lay (a
 * filter blends at full `Strength`, a generator's `Detail` starts at the noise's own).
 *
 * The tree always passes its kind's validator (#BKE_paint_layers_node_group_supports_filter and
 * its siblings): the shapes are the convention's own, asserted as a self-check, and a build that
 * somehow stopped short answers null instead of reaching a row.
 *
 * The tree is created for the caller to give a row: assign it with
 * #BKE_paint_layers_kind_tree_set, which takes a user of its own, then drop the creation user the
 * new tree came with once a row reads it. A caller that cannot complete the row frees the tree
 * instead, so a half-made definition never sits orphaned in the file.
 *
 * Only the Add operator calls this today. The layer and the filter are authored through it, and
 * the generic Add and `kind_tree` paths keep a row without a definition inert on purpose: a script
 * may want to assign a tree of its own, and the operator is the UI path. Seeding a template when
 * the properties panel flips a row to the Node Group generator is planned with the generator
 * patch; this build's generator kind setter refuses the flip (no registry entry yet), so no inert
 * row is left behind.
 *
 * \param name: the tree's name, taken as given (the database uniquifies it); null or empty falls
 * back to "Node Group", the operator's own default.
 * \return the new tree, or null when it could not be built; the caller leaves its row inert then,
 * which the row's consumers read as a missing definition.
 */
bNodeTree *BKE_paint_layers_node_group_template_ensure(Main &bmain,
                                                       PaintLayerNodeGroupTemplateKind kind,
                                                       const char *name);

/**
 * Which paint channels a node-group source tree answers, read off its outputs: an output named
 * after a paint channel (`MaterialPaintChannelInfo::ui_name`, "Base Color", "Roughness", ...)
 * fills that channel, and a tree that names at least one is a per-channel source -- every channel
 * it does not name takes no part, instead of reading the one `Result` map whole.
 *
 * A channel the Principled cannot carry (a Height, an AO, a Custom) is never named here even when
 * an output wears its name: the bake prints what the Principled feeds, so a tree that answers a
 * mapless channel has no answer the first pass can lay. The channels that remain are the
 * per-channel answers the shader-bake computes, one map each.
 *
 * \param r_channels: one bool per paint channel (#PAINT_MATERIAL_CHANNEL_NUM entries), set to
 * whether the tree answers that channel. False return means the tree names none of them -- a
 * `Result`-only tree on the one-answer contract, every channel reading the same map.
 */
bool BKE_paint_layers_node_group_source_channels(const bNodeTree *tree,
                                                 MutableSpan<bool> r_channels);

/** \} */

/* -------------------------------------------------------------------- */
/** \name The interface hash
 *
 * The identity of what a tree exposes: every input and output's name, type, default, range and
 * structure. The dynamic schema, the bake cache and the bake hash key on it, so an interface edit
 * invalidates every derived answer of the trees' rows without walking a parameter. A value edit on
 * a node inside the tree does not move it -- the values are the rows' own parameters by then.
 * \{ */

/**
 * The interface hash of \a tree, or 0 for a null tree. Memoized against the tree's update signal
 * (the way the source graph's hash is), so repeated asks between edits are one comparison.
 */
uint64_t BKE_paint_layers_node_group_interface_hash(const bNodeTree *tree);

/**
 * Drop every memo keyed on \a tree: the tree was edited through a path the update signal does not
 * cover (an interface edit through RNA before the tree update runs). The tree's own memo entries
 * and every descriptor built from it are retired -- kept allocated until the next
 * #BKE_paint_layers_node_group_forget_all, because a reader may still be walking one -- and the
 * next ask rebuilds them. Safe on a null pointer, safe to call for a tree that carries none.
 */
void BKE_paint_layers_node_group_forget(const bNodeTree *tree);

/**
 * Drop the chain descriptors keyed on \a material_session_uid: the definition material was freed,
 * so nothing can name it again and its aggregate schema would be a dead entry the whole session.
 * Called from the material's own free, the way the shader-bake's per-material clear is; the tree
 * forget above cannot reach these, because a chain is keyed on a material, not on a tree. Retired
 * rather than freed for the same reason the tree forget retires.
 */
void BKE_paint_layers_node_group_forget_chain(uint32_t material_session_uid);

/**
 * Drop every memo of every tree and every chain definition. Called on the file-load post, with
 * the other memo clears -- the one point a retired descriptor dies, because no reader can hold a
 * pointer into the previous file's definitions any more.
 */
void BKE_paint_layers_node_group_forget_all();

/**
 * The hash of the whole definition a row reads -- for a node-group kind the tree's identity and
 * interface hash folded with the tree's content revision (a node value, a link, an interface item
 * or a nested group's edit; the revision the tree update raises, mixed with the topology hash and
 * the nested trees' own signals), for a chain the definition's identity and stack generation --
 * and the row's kind. The bake and the composite hash it into their keys together with the row's
 * stored values, so an edit of the definition (its interface, its graph, or the values inside its
 * nodes) invalidates exactly the instances that read it, and a row re-pointed at another
 * definition never answers with the previous one's pixels. Zero for a row whose definition is
 * gone.
 */
uint64_t BKE_paint_layers_node_group_definition_hash(const MaterialPaintLayer &row);

/**
 * Resync every row of \a bmain that reads one of \a trees: re-seed its parameters from the
 * definition's current interface (a socket the tree gained gets its default stored, one it lost
 * keeps its stored value harmlessly) and tag the owning material, so the composite and the bake
 * see the new schema. Called from the tree update, which is the one moment an interface edit is
 * known to have settled; an edit that moved nothing answers without touching anything.
 */
void BKE_paint_layers_node_group_rows_resync(Main &bmain, Span<bNodeTree *> trees);

/** \} */

}  // namespace blender
