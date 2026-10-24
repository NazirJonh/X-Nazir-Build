/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Internal types shared by the paint-layer generator's build translation units. Everything here is
 * private to the generator; the public entry point stays `BKE_paint_layers_tree_build` in
 * #BKE_paint_layers_generate.hh.
 */

#include "BKE_node_tree_interface.hh"

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_listBase.h"
#include "DNA_uuid_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"

#include <memory>
#include <utility>

namespace blender {

struct Image;
struct Material;
struct MaterialPaintLayer;
struct PaintLayersBuildContext;
struct bNode;
struct bNodeSocket;
struct bNodeTree;
struct bNodeTreeInterfaceSocket;

namespace bke::paint_layers {

/**
 * A row's own `.PL Layer` node group, its Group Input/Output nodes, and its instance in the root
 * tree. A row builds every channel it takes part in into the same group.
 */
struct LayerGroup {
  bNodeTree *tree = nullptr;
  bNode *group_input = nullptr;
  bNode *group_output = nullptr;
  bNode *instance = nullptr;
  bNodeTree *parent_tree = nullptr;
};

/**
 * One built row of one channel: its instance in the parent and the sockets the parent chains.
 *
 * A row inside a folder works on the folder's pre-multiplied accumulation, which needs the coverage
 * accumulated so far next to the colour; a folder row also takes the isolated result of what it
 * holds. Those sockets are null for a row that has no use for them.
 */
struct RowResult {
  bool valid = false;
  bNode *instance = nullptr;
  /** The instance socket the chain below feeds. */
  bNodeSocket *below = nullptr;
  /** The instance socket carrying the row laid over what is below. */
  bNodeSocket *result = nullptr;
  /** Inside a folder: the coverage accumulated below, and the one with this row laid over it. */
  bNodeSocket *below_coverage = nullptr;
  bNodeSocket *result_coverage = nullptr;
  /** A folder: its contents' straight colour and coverage, the source and factor base of the row. */
  bNodeSocket *folder_color = nullptr;
  bNodeSocket *folder_coverage = nullptr;
  /**
   * The content alpha of a channel that tracks one: what is below (pre-multiplied inside a folder),
   * the one with this row laid over it, and a folder's straightened alpha of what it holds. Null
   * for a channel that tracks none.
   */
  bNodeSocket *below_alpha = nullptr;
  bNodeSocket *result_alpha = nullptr;
  bNodeSocket *folder_alpha = nullptr;
};

/** A node and one of its output sockets: what the next stage of a chain reads from. */
struct NodeOutput {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
};

/**
 * One Anchor Point tap: the outputs the marker was given in its row's group. The marker publishes
 * the content of its row up to its position (channel >= 0) or the mask up to its position
 * (channel == -1). A reader in the same row reads the outputs directly; a reader in another row
 * finds the same interface sockets on the source group's instance.
 */
struct AnchorPointTap {
  bUUID point = {};
  int channel = -1;
  /** The group the marker lives in; the outputs below are nodes of its tree, not the Group Output
   * sockets, which only publish them to readers in other rows. */
  const LayerGroup *group = nullptr;
  NodeOutput color;
  NodeOutput coverage;
  NodeOutput alpha;
};

/** The interface inputs of one Color ID colour: its enable flag, its RGB constant and, in Advanced
 * mode, its own tolerance. */
struct IdColorInputs {
  bNodeTreeInterfaceSocket *enabled = nullptr;
  bNodeTreeInterfaceSocket *color = nullptr;
  bNodeTreeInterfaceSocket *tolerance = nullptr;
};

/**
 * What a chain has accumulated so far: its colour and, inside a folder, the coverage. The coverage
 * is empty on the stack's own chain, which blends straight over an opaque bottom.
 */
struct ChainEnd {
  NodeOutput color;
  NodeOutput coverage;
  /** The content alpha of a channel that tracks one; empty otherwise. Pre-multiplied (`P_a`) inside
   * a folder, like the colour. */
  NodeOutput alpha;
};

/**
 * What a kind emits as the source of a row in one channel: the straight colour, the coverage that
 * colour has of its own, and the content alpha of a channel that tracks one. A null socket is a
 * part the kind has not got: no coverage means it covers fully, no alpha that the channel tracks
 * none. A null colour is a failure, and nothing was left behind.
 */
struct KindEmitResult {
  NodeOutput color;
  NodeOutput coverage;
  NodeOutput content_alpha;
};

/** What a row's group offers its kind to build its source from. */
struct RowEmitContext {
  bNodeTree &tree;
  LayerGroup &group;
  const MaterialPaintLayer &layer;
  int channel;
  /** The row is a folder, an adjustment, or a Paint row kept only for its corrections. */
  bool folder;
  bool adjustment;
  bool empty_base;
  /** The channel tracks a content alpha, so the group has the sockets below. */
  bool track_alpha;
  /** The row's record in the channel; null for a folder and an adjustment. */
  const MaterialPaintLayerChannel *record;
  /** The group input sockets of what is below, and of a folder's contents. */
  bNodeSocket *below;
  bNodeSocket *below_alpha;
  bNodeSocket *folder_color;
  bNodeSocket *folder_alpha;
  /** The row's bake in this channel when its bake stands in, else null: the source is then that map. */
  Image *baked_color;
};

/** The blend of one row over what is below: a Normal Combine instance or a Mix node. */
struct BlendNode {
  bNode *node = nullptr;
  /** What is below. */
  bNodeSocket *a = nullptr;
  /** The row's source. */
  bNodeSocket *b = nullptr;
  bNodeSocket *factor = nullptr;
  bNodeSocket *result = nullptr;
};

/**
 * The state one `BKE_paint_layers_tree_build` call carries: the description and the tree it builds
 * into, plus the registries the row builder fills and reads across channels.
 */
class PaintLayersTreeBuilder {
 public:
  PaintLayersTreeBuilder(const Material &ma, bNodeTree &tree, const PaintLayersBuildContext &ctx)
      : ma_(ma), tree_(tree), ctx_(ctx), forms_(paint_layers_form_map(ma))
  {
  }

  void build();

 private:
  /** The group of \a layer, created on first use: its tree, its instance and its value inputs. */
  LayerGroup *layer_group_ensure(const MaterialPaintLayer &layer, float location_x, float location_y);

  /** Add the Group Input and Output nodes to the empty tree of \a group, at their places. False
   * when a node cannot be made. */
  bool layer_group_io_nodes_add(LayerGroup &group);

  /** The value inputs of \a layer: one opacity per wired channel the row takes part in, and the
   * constant of every map-less channel; plus the base mask's opacity and, while the mask has no
   * map, its constant. Their values are written where they are created. */
  void create_value_inputs(LayerGroup &group, const MaterialPaintLayer &layer);

  /**
   * The float input that picks the content alpha model of \a layer in \a channel: 1 for a Mix row,
   * 0 otherwise. A value, so a blend edit never rewires the alpha chain.
   */
  void blend_mix_input_create(LayerGroup &group,
                              const MaterialPaintLayer &layer,
                              int channel,
                              const char *layer_name);

  /**
   * The factor of \a layer's base mask, built once in the row's group and shared by every channel
   * chain: `mix(1, C, A * opacity)`, with `C` the grey of the mask map (or the constant input) and
   * `A` the map's alpha. Empty when the row has no mask, or its inputs are not there.
   */
  NodeOutput mask_factor_ensure(bNodeTree &tree, LayerGroup &group, const MaterialPaintLayer &layer);

  /**
   * Expose the row's mask factor as the group's `Mask` output, a grayscale colour. Built once per
   * row, when its first channel is built; a row without a mask gets no output.
   */
  void mask_output_ensure(LayerGroup &group, const MaterialPaintLayer &layer);

  /** Link the root's `Result Mask` output to the active row's `Mask` output after the channels. */
  void root_mask_output_link();

  /**
   * The row's 0/1 visibility on the object being drawn, read from that object's mask record by an
   * Attribute node, built once per row and shared by its channels. Empty when the row has no
   * object mask or the nodes cannot be made. Mesh reads the object's flag; UV Tiles reads the tile
   * bit of the fragment's UDIM.
   */
  NodeOutput object_mask_visible_ensure(bNodeTree &tree, const MaterialPaintLayer &layer);

  /**
   * One row of one channel: its group contents, and the instance sockets the root chains.
   *
   * \param isolated: the row hangs inside a folder, so it works on the folder's pre-multiplied
   * accumulation -- colour `P` and coverage `a` -- instead of blending straight over an opaque
   * bottom.
   */
  RowResult build_row(const MaterialPaintLayer &layer,
                      LayerGroup &group,
                      int channel,
                      bool isolated);

  /**
   * Lay the content correction \a effect over \a content, the straight colour the row has so far in
   * \a channel, building its nodes into \a group's tree. \a coverage is the coverage the row's own
   * content has, or empty while it covers fully; both move on to the result, and so does
   * \a content_alpha -- the row's content alpha, raised by the over model -- when the channel
   * tracks one (null otherwise). False (and nothing built) when the correction lays no map in
   * \a channel.
   */
  bool correction_lay(bNodeTree &tree,
                      LayerGroup &group,
                      const MaterialPaintLayer &effect,
                      int channel,
                      NodeOutput &content,
                      NodeOutput &coverage,
                      NodeOutput *content_alpha);

  /**
   * Apply the filter correction \a effect to \a content, the straight colour the row has so far in
   * \a channel, building its nodes into \a group's tree. False (and nothing built) when the filter
   * does not act in \a channel.
   */
  bool filter_lay(bNodeTree &tree,
                  LayerGroup &group,
                  const MaterialPaintLayer &effect,
                  int channel,
                  NodeOutput &content,
                  bool full_strength = false);

  /**
   * The source of the row \a ctx names, built into its group: the one place the generator tells
   * the kinds of a row apart. A kind added later is a branch here and nowhere else in the row's
   * build.
   */
  KindEmitResult row_source_emit(const RowEmitContext &ctx);

  /**
   * The `match` a map-coverage mask item \a item lays, built into its group: for UV Island the red
   * channel of its map, for Color ID the best match over its colours. The map is read through an
   * Image Texture with Closest interpolation and its Mapping; a missing map is a constant zero, so
   * the operator restricts everything as the CPU reads it. Nodes made are appended to \a r_nodes.
   */
  NodeOutput map_coverage_match_add(bNodeTree &tree,
                                    LayerGroup &group,
                                    const MaterialPaintLayer &item,
                                    Vector<bNode *> &r_nodes);

  /**
   * The instance sockets of a row whose group was kept unchanged: its nodes and interface already
   * hold the row, so only the contract sockets are looked up again in the new instance.
   */
  RowResult row_link_reused(const MaterialPaintLayer &layer,
                            LayerGroup &group,
                            int channel,
                            bool isolated);

  /**
   * Throw away a kept group's nodes (its interface stays) and build \a layer's row again. Used when
   * a kept reader group predates an input the row now needs; the alternative is a row silently
   * missing from the chain. Clears this builder's caches that named nodes inside the old group.
   *
   * \param r_group_ok: whether the group could be restored (every channel it held, and a valid Group
   * Input/Output). An invalid result with a restored group only means \a channel is not one the row
   * can lay, which is the plain skip of a channel; without it the group is broken.
   */
  RowResult row_rebuild_kept(const MaterialPaintLayer &layer,
                             LayerGroup &group,
                             int channel,
                             bool isolated,
                             bool &r_group_ok);

  /**
   * Drop every builder cache entry that names \a layer (or taps published by \a group): the nodes
   * they point at are about to be cleared with a rebuild.
   */
  void row_caches_forget(const MaterialPaintLayer &layer, const LayerGroup &group);

  /**
   * Chain the rows of \a list over \a end for \a channel, recursing into folders. A folder builds
   * its children over an empty accumulation and then lays the straightened result over what is
   * below with its own blend, opacity and mask. \a location_x advances past what was built.
   */
  ChainEnd list_build(const ListBaseT<MaterialPaintLayer> &list,
                      ChainEnd end,
                      int channel,
                      float location_y,
                      float &location_x,
                      bool isolated);

  /**
   * The blend node of \a row in \a channel: a Normal Combine instance for Normal when the shared
   * group exists and the channel has not been set to Replace, a Mix otherwise. Empty when its
   * sockets cannot be found.
   */
  BlendNode blend_node_add(bNodeTree &tree,
                           const MaterialPaintLayer &row,
                           int channel,
                           int ramp_blend);

  /** `a * b` as a Math node in \a tree; empty when it cannot be built. */
  NodeOutput multiply_nodes_add(bNodeTree &tree, NodeOutput a, NodeOutput b);

  /**
   * What a Material row lays in the channel of \a ctx: its source's plain value (an input of the
   * group), its source's plain Image Texture, or the map rendered from it, with the coverage its
   * source's transparency gives. Empty when the row has nothing there.
   */
  KindEmitResult material_source_emit(const RowEmitContext &ctx);

  /**
   * What an Anchor Point Reader lays in the channel of \a ctx: the tap the marker published, read
   * from this build's taps when the point is in the same row, or from the source row group's
   * instance otherwise. Empty when the reference is invalid.
   */
  KindEmitResult anchor_point_source_emit(const RowEmitContext &ctx);

  /** The tap of \a point in \a channel (-1 for the mask) published by \a group itself, or null. A
   * tap of another row's group is not readable here: its nodes live in another tree. */
  const AnchorPointTap *anchor_point_tap_find(const LayerGroup &group,
                                              const bUUID &point,
                                              int channel) const;

  /**
   * Publish the content of \a group up to \a point as Group Output sockets and record the tap.
   * Returns false when the sockets cannot be made.
   */
  bool anchor_point_content_tap_add(bNodeTree &tree,
                                    LayerGroup &group,
                                    const MaterialPaintLayer &point,
                                    int channel,
                                    NodeOutput color,
                                    NodeOutput coverage,
                                    NodeOutput alpha);

  /** Publish the mask of \a group up to \a point as a Float Group Output socket. */
  bool anchor_point_mask_tap_add(bNodeTree &tree,
                                 LayerGroup &group,
                                 const MaterialPaintLayer &point,
                                 NodeOutput mask);

  /** The scalar a mask item that reads \a item lays in this row's mask. */
  NodeOutput anchor_point_mask_item_emit(LayerGroup &group, const MaterialPaintLayer &item);

  /**
   * Wire the source group's tap outputs to inputs of \a reader_group's instance in the root tree:
   * a reader in another row cannot link into a node of the source's tree, so the value crosses as
   * a group input. \a create_inputs adds the interface inputs when the row is built; a reused
   * reader group already has them and only needs the instance links again.
   */
  bool anchor_point_reader_link(LayerGroup &reader_group,
                                const MaterialPaintLayer &reader,
                                int channel,
                                bool create_inputs,
                                KindEmitResult *r_out);

  /**
   * What a Gradient row lays in the channel of \a ctx: the ColorRamp's colour and alpha for a
   * colour channel, a grey mix of the channel's two ends for a scalar one. The Factor group
   * instance and the ColorRamp node are shared by every channel of the row.
   */
  KindEmitResult gradient_source_emit(const RowEmitContext &ctx);

  /**
   * The row's instance of the shared Gradient Factor group, built on first use and shared by every
   * channel of the row. Its UV input reads the row group's Texture Coordinate; Start, End, Midpoint,
   * Type and Repeat read the row group's value inputs. Null while the build context has no gradient
   * group or a socket is missing.
   */
  NodeOutput gradient_factor_ensure(bNodeTree &tree,
                                    LayerGroup &group,
                                    const MaterialPaintLayer &layer);

  /**
   * The row's ColorRamp node, keyed by the row's marker and built on first use. Its ColorBand is a
   * copy of the description's ramp; a ramp edit regenerates the group, so this node is never
   * updated in place (its storage change would need a node-property tag the value sync does not
   * issue).
   */
  bNode *gradient_ramp_ensure(bNodeTree &tree,
                              LayerGroup &group,
                              const MaterialPaintLayer &layer);

  /**
   * The output of the Material row's source group for \a channel, instancing the group in \a tree
   * once for the row, outside the frames since every channel of the row shares it. Null when the
   * row has no source group or it has no output for the channel; the caller then reads the row's
   * map.
   */
  NodeOutput source_group_output_get(bNodeTree &tree,
                                     const MaterialPaintLayer &layer,
                                     int channel);

  /**
   * Wire the row group's mapping value inputs into \a instance, the source wrapper of \a layer:
   * the wrapper carries a Vector Rotate and a Mapping inside, and their offset/scale/rotation read
   * these three inputs. Idempotent, because every channel of the row shares the instance. A row
   * without an applied mapping adds no link and leaves the wrapper's inputs at their identity
   * defaults.
   */
  void source_group_mapping_wire(bNodeTree &tree,
                                 const MaterialPaintLayer &layer,
                                 bNode &instance);

  /**
   * An Image Texture node of \a tree reading \a image, holding one user of it, so every map source
   * is made the same way. Null when the node cannot be created; the caller places it.
   */
  bNode *image_map_node_add(bNodeTree &tree, Image &image);

  /**
   * The mean of \a color's RGB as a float: Separate XYZ, two Adds and a Divide by three, which is
   * what the CPU composite reads of a mask map. The nodes made are appended to \a r_nodes. Empty
   * when a node or socket is missing, in which case nothing was added.
   */
  NodeOutput rgb_mean_nodes_add(bNodeTree &tree, NodeOutput color, Vector<bNode *> &r_nodes);

  /**
   * \a color with its green channel flipped (`G' = 1 - G`): Separate XYZ, a Subtract from one and
   * Combine XYZ. Turns a DirectX tangent-space normal map into the OpenGL one the Normal Map node
   * reads. The nodes made are appended to \a r_nodes; empty when one is missing, in which case
   * nothing was added.
   */
  NodeOutput normal_green_flip_nodes_add(bNodeTree &tree,
                                         NodeOutput color,
                                         Vector<bNode *> &r_nodes);

  /**
   * The three UV mapping inputs of \a row (offset, scale, rotation), named after \a label, in
   * \a group's interface. Made only while the row's mapping applies, so toggling it changes the
   * interface once; a slider move syncs the values in place and never rebuilds.
   */
  void mapping_inputs_create(LayerGroup &group, const MaterialPaintLayer &row, const char *label);

  /**
   * Read \a map through the Mapping node of \a row: a `Point` Mapping fed by the object's UV and by
   * the row's mapping inputs, built on first use and shared by every map of the row. Tiling needs
   * Repeat, which is set on \a map. False, and \a map untouched, while the row's mapping does not
   * apply. A Mapping made by this call is appended to \a r_nodes, so it joins the caller's frame
   * and is removed with it when the caller rolls back.
   */
  bool mapping_apply(bNodeTree &tree,
                     const MaterialPaintLayer &row,
                     bNode &map,
                     Vector<bNode *> &r_nodes);

  /**
   * The Mapping node of \a row in \a tree: a Vector Rotate (the row's rotation) followed by a
   * `Point` Mapping (the row's scale and offset), fed by the object's UV and by the row group's
   * mapping inputs. Built on first use and shared by every map of the row; \a r_nodes receives the
   * nodes this call made, so a caller that rolls back removes them. Null while the row's mapping
   * does not apply or a node cannot be made.
   *
   * A Material row whose source is shown through its wrapper does not read a map here: the wrapper
   * carries its own Mapping, and #source_group_mapping_wire hands it the row's values. The clip
   * factor still builds this node for such a row, so the factor sees the same transform.
   */
  bNode *mapping_node_ensure(bNodeTree &tree,
                             const MaterialPaintLayer &row,
                             Vector<bNode *> &r_nodes);

  /**
   * The clip factor of \a row's UV wrap: one inside the unit square, zero outside along every axis
   * the wrap clips, or an empty output while both axes repeat or the mapping does not apply. The
   * nodes made are appended to \a r_nodes. The factor is shared by every channel of the row.
   */
  NodeOutput mapping_clip_factor(bNodeTree &tree,
                                 const MaterialPaintLayer &row,
                                 Vector<bNode *> &r_nodes,
                                 bool create_mapping = true);

  /**
   * Remove \a nodes from \a tree, forgetting any row Mapping among them. A Mapping made by an
   * earlier caller is not in \a nodes and stays.
   */
  void nodes_discard(bNodeTree &tree, Span<bNode *> nodes);

  /**
   * \a alpha, the Alpha of a map that \a row reads through its Mapping, times the clip factor of
   * the row's UV wrap: one inside the unit square, zero outside along every axis the wrap clips.
   * The CPU resample leaves such texels transparent, and every reader of the map's alpha (the row's
   * coverage and content alpha, a correction's factor, a mask item's weight) takes it from here.
   * \a alpha unchanged while the wrap repeats on both axes, the mapping does not apply, or the row
   * built no Mapping (call #mapping_apply first). The nodes made are appended to \a r_nodes.
   */
  NodeOutput mapping_clip_alpha(bNodeTree &tree,
                                const MaterialPaintLayer &row,
                                NodeOutput alpha,
                                Vector<bNode *> &r_nodes);

  /** The Texture Coordinate node of \a tree whose `UV` output feeds a Mapping, made once. */
  bNode *uv_coordinate_ensure(bNodeTree &tree);

  /**
   * Re-orient a tangent-space Normal map that \a row reads through its Mapping: returns the
   * socket to use in place of \a color. The Mapping moved only the read point, so the encoded
   * vectors are decoded, rotated back by the same Rotation and flipped by the sign of the same
   * Scale, and encoded again. \a color unchanged when the row builds no Mapping. The values ride
   * the Mapping's own inputs, so editing them never recompiles the shader. The nodes made are
   * appended to \a r_nodes when given.
   */
  NodeOutput normal_remap_add(bNodeTree &tree,
                              const MaterialPaintLayer &row,
                              NodeOutput color,
                              Vector<bNode *> *r_nodes = nullptr);

  /** The interface and the group output node; false when the output node cannot be created. */
  bool result_interface_create();

  /** The RGB constant a channel's chain starts from, at \a location_y. */
  NodeOutput channel_bottom_add(int channel, float location_y);

  /** The rows of \a channel chained over \a below; \a location_x advances past what was built. */
  ChainEnd channel_chain_build(int channel, ChainEnd below, float location_y, float &location_x);

  /**
   * Decode, normalize and encode the Normal channel at \a from: a partial factor interpolates the
   * combine's encoded output against the base, which shortens the vector, so one final pass
   * restores a unit normal, matching what the shader's Normal Map node hands the BSDF.
   */
  NodeOutput normal_renormalize_add(NodeOutput from, float location_x, float location_y);

  /** The Group Input socket of \a group that mirrors interface socket \a iface, or null. */
  bNodeSocket *group_input_socket(const LayerGroup &group,
                                  const bNodeTreeInterfaceSocket &iface) const;

  const Material &ma_;
  bNodeTree &tree_;
  const PaintLayersBuildContext &ctx_;
  /** The place-dependent form of every row, walked once per build. */
  PaintLayerFormMap forms_;
  Vector<int> wired_channels_;

  /** A row's opacity input per channel, keyed by the row. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> opacity_inputs_;
  /** A row's "is Mix" input per channel, keyed by the row. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> blend_mix_inputs_;
  /** A content correction's opacity input per channel, keyed by the correction. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> effect_opacity_inputs_;
  /** A filter correction's parameter inputs in its kind's schema order, keyed by the correction. */
  Map<const MaterialPaintLayer *, Vector<bNodeTreeInterfaceSocket *>> effect_param_inputs_;
  /** The interface inputs of every Color ID colour of an item, in list order, keyed by the item. */
  Map<const MaterialPaintLayer *, Vector<IdColorInputs>> id_color_inputs_;
  /** The constant input of a row's map-less channel, keyed by the row and the channel. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> fill_inputs_;
  /** The alpha of that constant, its own float input, keyed the same way. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> fill_alpha_inputs_;
  /** The plain value a Material row's source supplies in a channel, keyed by the row and channel.
   */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> live_constant_inputs_;
  /** The coverage of a Material row whose source's Alpha is a plain value, keyed by the row. */
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> live_coverage_inputs_;
  /** A row's base mask opacity and constant inputs, keyed by the row. */
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mask_opacity_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mask_value_inputs_;
  /** A row's UV mapping value inputs, keyed by the row that owns the Mapping. */
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mapping_offset_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mapping_scale_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mapping_rotation_inputs_;
  /** The Mapping node built for a row, so every channel and map of it shares the one node. */
  Map<const MaterialPaintLayer *, bNode *> mapping_nodes_;
  /** The instance of a Material row's source group, with the group, keyed by the row. */
  Map<const MaterialPaintLayer *, std::pair<bNode *, bNodeTree *>> source_group_instances_;
  /** The taps this build published, one per (point, channel) and one per point for the mask. */
  Vector<AnchorPointTap> anchor_point_taps_;
  /** A Gradient row's Factor group instance, shared by every channel of the row. */
  Map<const MaterialPaintLayer *, NodeOutput> gradient_factors_;
  /** A Gradient row's ColorRamp node, shared by every colour channel of the row. */
  Map<const MaterialPaintLayer *, bNode *> gradient_ramps_;
  /** A Gradient row's line, midpoint, type and repeat value inputs. */
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> gradient_start_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> gradient_end_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> gradient_midpoint_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> gradient_type_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> gradient_repeat_inputs_;
  /** A scalar channel's two gradient ends, keyed by the row and the channel. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> gradient_value_start_inputs_;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> gradient_value_end_inputs_;
  /** The clip factor of a row's UV wrap, built once and shared by every map of the row. */
  Map<const MaterialPaintLayer *, NodeOutput> mapping_clip_masks_;
  /** A row's built mask factor, so the second channel reads the nodes the first one made. */
  Map<const MaterialPaintLayer *, NodeOutput> mask_factors_;
  /** The rows whose group already exposes their mask output. */
  Set<const MaterialPaintLayer *> mask_outputs_;
  /** A row's built object-mask visibility, shared the same way. */
  Map<const MaterialPaintLayer *, NodeOutput> object_mask_visible_;
  Map<int, bNodeTreeInterfaceSocket *> result_outputs_;
  /** The root's mask output, fed by the active row's group after the channels are built. */
  bNodeTreeInterfaceSocket *mask_output_ = nullptr;
  bNode *group_output_ = nullptr;
  /** Node positions of this build; a fresh builder starts from row zero. */
  layout::LayoutGrid grid_;
  Map<const MaterialPaintLayer *, LayerGroup *> layer_groups_;
  Vector<std::unique_ptr<LayerGroup>> layer_group_storage_;
  /**
   * Rows whose kept group was torn down and rebuilt in this pass. A group is shared by every
   * channel of the row: rebuild once on the first channel that needs it, then call #build_row for
   * the rest without clearing again.
   */
  Set<const MaterialPaintLayer *> rebuilt_rows_;
  /** The channels each row's group has been built for in this pass: a rebuilt group holds the ones
   * built before the rebuild, and the later ones are built as their channel comes up. */
  Map<const MaterialPaintLayer *, Set<int>> built_channels_;
  /** Rows whose rebuild failed in this pass. The group left in the root cannot be grown again here,
   * so every later channel skips the row instead of building a half group on top of it. */
  Set<const MaterialPaintLayer *> failed_rows_;
};

/* -------------------------------------------------------------------- */
/** \name Layer-group interface helpers
 *
 * Stateless helpers over one #LayerGroup (and the interface it owns).
 * \{ */

/** Refresh the Group Input/Output and parent instance sockets of \a group after its interface grew. */
void refresh_layer_group(LayerGroup &group);

/** Add an interface socket of \a group and grow its three nodes to match; find it first, so a
 * rebuild of a group with a kept interface reuses the socket rather than duplicating it. */
bNodeTreeInterfaceSocket *layer_group_add_socket(LayerGroup &group,
                                                 const char *base,
                                                 StringRef socket_type,
                                                 NodeTreeInterfaceSocketFlag flag);

/** The interface socket of \a group named \a name of the given type and direction, or null. */
bNodeTreeInterfaceSocket *layer_group_socket_find(const LayerGroup &group,
                                                  const char *name,
                                                  StringRef socket_type,
                                                  bool input);

/**
 * Link the blend of one row: \a below into its first input, \a source into its second and
 * \a factor into its factor; a null factor socket makes the row cover fully.
 */
void blend_inputs_link(bNodeTree &tree,
                       const BlendNode &blend,
                       NodeOutput below,
                       NodeOutput source,
                       NodeOutput factor);

/**
 * Write \a value into \a iface's default and into the instance socket built from it, so the row
 * group's input and the instance that feeds it agree.
 */
void group_input_default_set(const LayerGroup &group, bNodeTreeInterfaceSocket &iface, float value);
void group_input_default_set(const LayerGroup &group,
                             bNodeTreeInterfaceSocket &iface,
                             const float value[4]);
/** As above for a Vector input; a name of its own, as `float[3]` and `float[4]` are one pointer. */
void group_input_vector_default_set(const LayerGroup &group,
                                    bNodeTreeInterfaceSocket &iface,
                                    const float value[3]);

/** Add a value input to \a group's interface, tagged with its role, layer and channel. */
bNodeTreeInterfaceSocket *layer_group_value_input(LayerGroup &group,
                                                  const char *base,
                                                  StringRef socket_type,
                                                  const char *role,
                                                  const bUUID &marker,
                                                  int channel);

/** \} */

}  // namespace bke::paint_layers
}  // namespace blender
