/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Internal types shared by the paint-layer generator's build translation units. Everything here is
 * private to the generator; the public entry point stays `paint_layers_tree_build` in
 * #paint_layers_generate.hh.
 */

#include "BKE_node_tree_interface.hh"
#include "BKE_paint_material_resolve.hh"

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_listbase.h"
#include "DNA_uuid_types.h"

#include <memory>
#include <string>
#include <utility>

namespace blender {

struct Image;
struct ImageUser;
struct MaterialPaintLayer;
struct MaterialPaintLayerChannel;
struct PaintLayersBuildContext;
struct PaintLayersRegenCache;
struct bNode;
struct bNodeSocket;
struct bNodeTree;
struct bNodeTreeInterfaceSocket;

namespace bke::paint_layers {

/**
 * One layer as the chain builder sees it: its source socket, and where its opacity comes from.
 *
 * The owner nodes are carried alongside the sockets because `bNodeSocket::owner_node()` reads a
 * runtime pointer that is only filled by the topology cache; the cache is stale while the tree is
 * being built, so asking a freshly created socket for its node would answer null.
 */
struct ChainLayer {
  const MaterialPaintLayer *layer = nullptr;
  bNode *source_node = nullptr;
  bNodeSocket *source = nullptr;
  /** The opacity interface input, before any per-pixel mask is folded in. */
  bNode *opacity_node = nullptr;
  bNodeSocket *opacity = nullptr;
  /** What the Mix/Combine factor links from: the opacity, or opacity times a mask map's alpha. */
  bNode *factor_node = nullptr;
  bNodeSocket *factor = nullptr;
  /** Float chain for content alpha (Base Color only). Null when not tracked (treat as 1.0). */
  bNode *content_alpha_node = nullptr;
  bNodeSocket *content_alpha = nullptr;
};

/** A chain's result plus, for a folder's contents, the coverage it accumulated. */
struct ChainResult {
  ChainLayer chain;
  /** The node owning #coverage; null for the root chain, which carries no coverage. */
  bNode *coverage_node = nullptr;
  bNodeSocket *coverage = nullptr;
  /** Scalar content alpha, parallel to coverage. Null when no Paint leaf contributes alpha. */
  bNode *content_alpha_node = nullptr;
  bNodeSocket *content_alpha = nullptr;
};

/**
 * Where a single row's nodes go: the tree and the group input its values are read from. A row's
 * body always builds inside its own layer group, so that group input carries the row's value inputs
 * (session 10e).
 */
struct RowTarget {
  bNodeTree *tree = nullptr;
  bNode *group_input = nullptr;
  float location_x = 0.0f;
  float location_y = 0.0f;
};

/** One built row: its source and factor, or invalid when the row takes part in nothing here. */
struct RowResult {
  bool valid = false;
  ChainLayer current;
  /** For a folder row, the coverage its contents accumulated; null otherwise. */
  bNode *folder_coverage_node = nullptr;
  bNodeSocket *folder_coverage = nullptr;
  /** For a folder row, the content alpha its contents accumulated; null otherwise. */
  bNode *folder_content_alpha_node = nullptr;
  bNodeSocket *folder_content_alpha = nullptr;
  /** True when the row was built inside its own layer group, not in the parent. */
  bool grouped = false;
  bNode *group_instance = nullptr;
  /** The instance sockets the parent chains through: Below in, Color/Coverage/Blend/Result out. */
  bNodeSocket *group_below = nullptr;
  bNodeSocket *group_color = nullptr;
  bNodeSocket *group_coverage = nullptr;
  bNodeSocket *group_content_alpha = nullptr;
  bNodeSocket *group_blend = nullptr;
  bNodeSocket *group_result = nullptr;
};

/** A leaf's or folder's own `.PL …` node group, its nodes, and its instance in the parent tree. */
struct LayerGroup {
  bNodeTree *tree = nullptr;
  bNode *group_input = nullptr;
  bNode *group_output = nullptr;
  bNode *instance = nullptr;
  bNodeTree *parent_tree = nullptr;
  /** The factory handed back a tree whose topology hash already matches: do not rebuild it. */
  bool unchanged = false;
  /**
   * The interface sockets this build created or reused. A rebuilt group's interface is not cleared
   * (so its socket identifiers, and the root's links into it, stay stable); anything not touched
   * this build is removed once the group is fully built.
   */
  Set<bNodeTreeInterfaceSocket *> used_sockets;
};

/**
 * What a row shows in a channel when its source is Material or Node Group: the same
 * Baked/Hybrid/SourceGroup resolution a Layer row's own channel content uses, shared by a Layer
 * row and an Effect correction of the same kind.
 */
struct RowMaterialSource {
  PaintLayerMaterialMode mode = PaintLayerMaterialMode::Baked;
  bool live_constant = false;
  float live_value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool live_map = false;
  Image *live_map_image = nullptr;
  const ImageUser *live_map_iuser = nullptr;
  bNode *source_group_instance = nullptr;
  bNodeTree *source_group_tree = nullptr;
  bNodeSocket *source_group_socket = nullptr;
};

/* -------------------------------------------------------------------- */
/** \name Layer-group interface helpers
 *
 * Stateless helpers over one #LayerGroup (and the interface it owns); they need none of the build's
 * shared registries, so they stay free functions rather than builder methods.
 * \{ */

/** Refresh the Group Input/Output and parent instance sockets of \a group after its interface grew. */
void refresh_layer_group(LayerGroup &group);

/** An existing interface socket of \a group with \a name, \a socket_type and direction, or null. */
bNodeTreeInterfaceSocket *group_interface_socket_find(LayerGroup &group,
                                                      const char *name,
                                                      StringRef socket_type,
                                                      NodeTreeInterfaceSocketFlag flag);

/** Add (or reuse) an interface socket of \a group and grow its three nodes to match. */
bNodeTreeInterfaceSocket *layer_group_add_socket(LayerGroup &group,
                                                 const char *base,
                                                 StringRef socket_type,
                                                 NodeTreeInterfaceSocketFlag flag);

/** Add a value input to \a group's interface, tagged so values_sync can find it. */
bNodeTreeInterfaceSocket *layer_group_value_input(LayerGroup &group,
                                                  const char *base,
                                                  StringRef socket_type,
                                                  const char *role,
                                                  const bUUID &marker,
                                                  int channel,
                                                  const bUUID &slot = {});

/** `marker|role|channel`, the key the mirror pass finds an existing value socket by. */
std::string value_key(const bUUID &marker, const char *role, int channel);

/**
 * The state one `paint_layers_tree_build` call carries: the description and the tree it builds into,
 * plus every registry the row/wrapper builders fill and read across channels. The per-channel chain
 * state stays local to #build.
 */
class PaintLayersTreeBuilder {
 public:
  PaintLayersTreeBuilder(const Material &ma, bNodeTree &tree, const PaintLayersBuildContext &ctx)
      : ma_(ma), tree_(tree), ctx_(ctx)
  {
  }

  void build();

 private:
  friend class PaintLayersChainBuilder;

  /** Relay every group's value inputs up to the root through the interfaces they pass. */
  void mirror_scope(bNodeTree &scope_tree,
                    bNode &scope_group_input,
                    Map<const bNodeTree *, LayerGroup *> &group_by_tree);
  /** Drop interface sockets a rebuilt group no longer uses, so its signature stays honest. */
  void prune_layer_group_sockets();
  /** Wire every created Image Texture to the one UV Map node the material names. */
  void wire_generated_uv_maps();

  bNode *source_group_instance_get(const MaterialPaintLayer &layer, bNodeTree &tree);
  /**
   * Wire the wrapper instance's mapping inputs from the row group's own (ТЗ 2.2): the row's
   * offset/scale/rotation feed the shared wrapper's Mapping. Nothing is built when the row's
   * mapping does not apply; the wiring itself is idempotent.
   */
  void source_group_mapping_wire(const MaterialPaintLayer &layer,
                                 bNodeTree &tree,
                                 bNode &instance);
  RowMaterialSource resolve_row_material_source(const MaterialPaintLayer &row,
                                                int channel,
                                                bNodeTree &row_tree,
                                                bool substituted);
  void create_value_inputs(LayerGroup &group, const MaterialPaintLayer &layer);
  LayerGroup *layer_group_ensure(const MaterialPaintLayer &layer, bNodeTree &parent_tree);
  /**
   * The row's Mapping `Vector` output, creating its node on first use: a `Point` Mapping whose
   * `Vector` reads the tree's one coordinate source and whose Location/Rotation/Scale read the
   * row group's mapping inputs. One node per row, shared by every channel and every map of it.
   * Null when the row builds no mapping (disabled, or nothing repeatable to remap).
   */
  std::pair<bNode *, bNodeSocket *> mapping_vector_ensure(bNodeTree &tree,
                                                          bNode *group_input,
                                                          const MaterialPaintLayer &row,
                                                          float location_x,
                                                          float location_y);
  /**
   * Re-orient a tangent-space Normal map read through the row's Mapping: returns the socket to
   * use in place of \a color. The Mapping only moves the read point, so the encoded vectors must
   * be rotated by the row's rotation and flipped by the sign of its scale to match. Returns
   * \a color unchanged when the row builds no Mapping. The topology is fixed (the values ride
   * the same group inputs as the Mapping), so editing Rotation/Scale never recompiles the shader.
   */
  bNodeSocket *normal_remap_ensure(bNodeTree &tree,
                                   const MaterialPaintLayer &row,
                                   bNode &color_node,
                                   bNodeSocket &color,
                                   bNode *&r_node,
                                   float location_x,
                                   float location_y);
  RowResult row_from_unchanged_group(LayerGroup &group,
                                     const MaterialPaintLayer &layer,
                                     int channel);

  const Material &ma_;
  bNodeTree &tree_;
  const PaintLayersBuildContext &ctx_;
  const PaintLayersRegenCache *cache_ = nullptr;
  Vector<int> wired_channels_;

  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> opacity_inputs_;
  /** A bake-substituted row's visibility input; the bake already holds its opacity. */
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> enabled_inputs_;
  Map<const MaterialPaintLayerChannel *, bNodeTreeInterfaceSocket *> fill_inputs_;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> correction_opacity_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> correction_fill_inputs_;
  /** A Fill correction's per-live-record constants: (correction, channel) -> its value input. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>>
      correction_fill_channel_inputs_;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> live_constant_inputs_;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>>
      correction_live_constant_inputs_;
  /** A mapped row's offset/scale/rotation value inputs, keyed by the row that owns the Mapping. */
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mapping_offset_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mapping_scale_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> mapping_rotation_inputs_;
  /** The Mapping node built for a row, so every channel and map of it shares the one node. */
  Map<const MaterialPaintLayer *, bNode *> mapping_nodes_;
  Map<int, bNodeTreeInterfaceSocket *> result_outputs_;
  bNode *group_input_ = nullptr;
  bNode *group_output_ = nullptr;
  RowTarget root_target_;
  Map<const MaterialPaintLayer *, LayerGroup *> layer_groups_;
  Vector<std::unique_ptr<LayerGroup>> layer_group_storage_;
  Map<const MaterialPaintLayer *, bNode *> source_group_instances_;
  Map<const MaterialPaintLayer *, bNodeTree *> source_group_trees_;
};

/** How #PaintLayersChainBuilder::build_mask_element_chain reduces an item's source to grey. */
enum class MaskElementGrey {
  /** The source socket is already a scalar: a Material/Node Group value or coverage. */
  AlreadyScalar,
  /** A scalar or atlas stored across R=G=B: Separate X reads it directly. */
  SeparateX,
  /** A full colour: the weighted mean (Separate + Add + Add + Divide). */
  Mean,
};

/**
 * One channel's chain: the two mutually recursive builders and the per-channel cursor they share.
 * A recursive call (a folder's children, a Stack correction/mask subtree) re-enters the *same*
 * object, so its `location_x` keeps advancing across the whole channel exactly as the captured
 * local did before the extraction.
 */
class PaintLayersChainBuilder {
 public:
  PaintLayersChainBuilder(PaintLayersTreeBuilder &outer, float location_x, float location_y)
      : outer_(outer), location_x_(location_x), location_y_(location_y)
  {
  }

  ChainResult build_list(const ListBaseT<MaterialPaintLayer> &list,
                         ChainLayer previous,
                         bool premul,
                         const RowTarget &parent_target,
                         int channel);

 private:
  RowResult build_row(const MaterialPaintLayer *layer,
                      const RowTarget &target,
                      bool substituted,
                      Image *baked_color,
                      bool premul,
                      LayerGroup *layer_group,
                      int channel);

  /**
   * A folder row's isolated sub-chain: builds its children and straightens the pre-multiplied
   * result into `S = P / a`. Fills the folder bookkeeping the caller keeps local to its row.
   * Returns false where the row drops out.
   */
  bool build_folder_source(const MaterialPaintLayer *layer,
                           bNodeTree &tree,
                           const RowTarget &target,
                           int channel,
                           bool track_content_alpha,
                           float location_x,
                           float location_y,
                           bNode **r_source_node,
                           bNodeSocket **r_source,
                           bNode **r_coverage_node,
                           bNodeSocket **r_coverage,
                           bNode **r_content_alpha_node,
                           bNodeSocket **r_content_alpha);

  /** The row's own channel source and opacity, filling \a r_current and \a r_leaf_map_node. */
  bool build_row_source(const MaterialPaintLayer *layer,
                        bNodeTree &tree,
                        bNode *group_input,
                        int channel,
                        bool substituted,
                        bool track_content_alpha,
                        float location_x,
                        float location_y,
                        const RowMaterialSource &row_source,
                        Image *baked_color,
                        bNode *folder_source_node,
                        bNodeSocket *folder_source,
                        bNode *folder_content_alpha_node,
                        bNodeSocket *folder_content_alpha,
                        ChainLayer &r_current,
                        bNode *&r_leaf_map_node);

  /** The substituted (baked) row's two Image Texture nodes and its coverage-derived opacity.
   * Variant C: when \a use_baked_content is set the baked color node is also returned in
   * \a r_content_node, so the factor chain reads per-channel content from its Alpha exactly
   * like a live leaf map; otherwise the row covers by its common factor alone. */
  bool build_substituted_source(const MaterialPaintLayer *layer,
                                bNodeTree &tree,
                                bool track_content_alpha,
                                float location_x,
                                float location_y,
                                Image *baked_color,
                                ChainLayer &r_current,
                                int channel,
                                bool use_baked_content,
                                bNode **r_content_node = nullptr);

  /** The factor base the mask stack builds on: the source's coverage, or one. */
  void resolve_row_factor(const MaterialPaintLayer *layer,
                          bNodeTree &tree,
                          bNode *group_input,
                          float location_x,
                          float location_y,
                          bool substituted,
                          const RowMaterialSource &row_source,
                          bNode *&r_factor_node,
                          bNodeSocket *&r_factor_socket);

  /** Content coverage, corrections, the mask stack, opacity and folder coverage, in order. */
  void build_row_factor_chain(const MaterialPaintLayer *layer,
                              const RowTarget &target,
                              bNodeTree &tree,
                              bNode *group_input,
                              int channel,
                              bool substituted,
                              bool track_content_alpha,
                              float location_x,
                              float location_y,
                              bNode *leaf_map_node,
                              bNode *&folder_coverage_node,
                              bNodeSocket *&folder_coverage,
                              ChainLayer &current,
                              bNode *&r_factor_node,
                              bNodeSocket *&r_factor_socket);

  /** Expose the row's Color/Coverage/Blend/Result on its own group and parent-tree instance. */
  bool build_grouped_row_result(const MaterialPaintLayer *layer,
                                bNodeTree &tree,
                                bNode *group_input,
                                LayerGroup *layer_group,
                                int channel,
                                bool premul,
                                float location_x,
                                float location_y,
                                ChainLayer &current,
                                RowResult &r_result);

  /** The Group Input socket of \a group_input that mirrors interface socket \a iface, or null. */
  bNodeSocket *group_input_socket(bNode *group_input, const bNodeTreeInterfaceSocket &iface);

  /** The mean of \a image's RGB as a new node chain (the way the CPU reads a mask/coverage map). */
  std::pair<bNode *, bNodeSocket *> build_grey_of_map(bNodeTree &tree,
                                                     float location_x,
                                                     float location_y,
                                                     Image &image,
                                                     float offset_y);

  /** The Effect corrections of \a layer, adjusting \a current's colour before its own blend. */
  void build_content_correction(const MaterialPaintLayer *layer,
                                const RowTarget &target,
                                bool substituted,
                                int channel,
                                bNodeTree &tree,
                                bNode *group_input,
                                float location_x,
                                float location_y,
                                bool track_content_alpha,
                                ChainLayer &current,
                                bNode *&content_cov_node,
                                bNodeSocket *&content_cov,
                                bNode *&folder_coverage_node,
                                bNodeSocket *&folder_coverage);

  /** The Material/Node Group correction's own coverage, on the Alpha channel. */
  std::pair<bNode *, bNodeSocket *> resolve_correction_coverage(bNodeTree &tree,
                                                                bNode *group_input,
                                                                float location_x,
                                                                float location_y,
                                                                const MaterialPaintLayer &row);

  /**
   * Configure a Hybrid Material row's live Image Texture the way a Layer row does: the source
   * node's sampling settings travel with it, then a mapped row reads through its own Mapping
   * (Repeat forced). `channel` picks which source image's settings are copied.
   */
  void live_map_configure(bNode &map,
                          bNodeTree &tree,
                          bNode *group_input,
                          const MaterialPaintLayer &row,
                          int channel,
                          float location_x,
                          float location_y);

  /** Materialize the factor's neutral base (one) when the mask stack needs one. */
  void ensure_factor_base(bNodeTree &tree,
                          float location_x,
                          float location_y,
                          bNode *&layer_factor_node,
                          bNodeSocket *&layer_factor_socket);

  /** The Mask Items of \a layer: a coverage stack over \a layer_factor_node/socket. */
  void build_mask_item(const MaterialPaintLayer *layer,
                       int channel,
                       bool substituted,
                       bNodeTree &tree,
                       bNode *group_input,
                       float location_x,
                       float location_y,
                       const RowTarget &target,
                       ChainLayer &current,
                       bNode *&layer_factor_node,
                       bNodeSocket *&layer_factor_socket);

  /**
   * The shared body of one mask element, built on already resolved sockets: reduce \a gray_source
   * to grey (see \a grey_mode), blend it over the running factor with the item's blend mode, fold
   * the item's opacity -- and, when \a multiply_socket is set, the map alpha or the source's own
   * coverage -- into the Mix factor, and straighten a data map by its alpha. The flat and packed
   * mask paths both route through here, so node order, `custom1` values, labels and grid rows stay
   * identical between them.
   *
   * \a multiply_socket null means the factor is the opacity alone (a Fill, a MeshMap atlas, or the
   * Alpha channel whose grey already is that number). \a straighten_grey builds the data-map
   * Divide, fed by \a map_node / \a map_alpha. Returns false where a required socket or node is
   * missing, so the caller skips the element exactly as its own null checks did.
   */
  bool build_mask_element_chain(bNodeTree &tree,
                                const MaterialPaintLayer &correction,
                                int mask_base_row,
                                MaskElementGrey grey_mode,
                                bNode *gray_source_node,
                                bNodeSocket *gray_source_color,
                                bNode *opacity_node,
                                bNodeSocket *opacity_socket,
                                bNode *multiply_node,
                                bNodeSocket *multiply_socket,
                                bNode *map_node,
                                bNodeSocket *map_alpha,
                                bool fill,
                                bool straighten_grey,
                                Vector<bNode *> &frame_nodes,
                                bNode *&factor_node,
                                bNodeSocket *&factor_socket);

  PaintLayersTreeBuilder &outer_;
  float location_x_;
  float location_y_;
};

}  // namespace bke::paint_layers
}  // namespace blender
