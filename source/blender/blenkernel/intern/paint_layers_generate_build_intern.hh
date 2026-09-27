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
                                                  int channel);

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
  RowMaterialSource resolve_row_material_source(const MaterialPaintLayer &row,
                                                int channel,
                                                bNodeTree &row_tree,
                                                bool substituted);
  void create_value_inputs(LayerGroup &group, const MaterialPaintLayer &layer);
  LayerGroup *layer_group_ensure(const MaterialPaintLayer &layer, bNodeTree &parent_tree);
  RowResult row_from_unchanged_group(LayerGroup &group,
                                     const MaterialPaintLayer &layer,
                                     int channel);

  const Material &ma_;
  bNodeTree &tree_;
  const PaintLayersBuildContext &ctx_;
  const PaintLayersRegenCache *cache_ = nullptr;
  Vector<int> wired_channels_;

  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> opacity_inputs_;
  Map<const MaterialPaintLayerChannel *, bNodeTreeInterfaceSocket *> fill_inputs_;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> correction_opacity_inputs_;
  Map<const MaterialPaintLayer *, bNodeTreeInterfaceSocket *> correction_fill_inputs_;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> live_constant_inputs_;
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>>
      correction_live_constant_inputs_;
  Map<int, bNodeTreeInterfaceSocket *> result_outputs_;
  bNode *group_input_ = nullptr;
  bNode *group_output_ = nullptr;
  RowTarget root_target_;
  Map<const MaterialPaintLayer *, LayerGroup *> layer_groups_;
  Vector<std::unique_ptr<LayerGroup>> layer_group_storage_;
  Map<const MaterialPaintLayer *, bNode *> source_group_instances_;
  Map<const MaterialPaintLayer *, bNodeTree *> source_group_trees_;
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

  /** The substituted (baked) row's two Image Texture nodes and its coverage-derived opacity. */
  bool build_substituted_source(const MaterialPaintLayer *layer,
                                bNodeTree &tree,
                                bool track_content_alpha,
                                float location_x,
                                float location_y,
                                Image *baked_color,
                                ChainLayer &r_current);

  /** The factor base the mask stack builds on: the source's coverage, or one. */
  void resolve_row_factor(const MaterialPaintLayer *layer,
                          bNodeTree &tree,
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
                                                                float location_x,
                                                                float location_y,
                                                                const MaterialPaintLayer &row);

  /** Materialize the factor's neutral base (one) when the mask stack needs one. */
  void ensure_factor_base(bNodeTree &tree,
                          float location_x,
                          float location_y,
                          bNode *&layer_factor_node,
                          bNodeSocket *&layer_factor_socket);

  /** The Mask Items of \a layer: a coverage stack over \a layer_factor_node/socket. */
  void build_mask_item(const MaterialPaintLayer *layer,
                       int channel,
                       bNodeTree &tree,
                       bNode *group_input,
                       float location_x,
                       float location_y,
                       const RowTarget &target,
                       ChainLayer &current,
                       bNode *&layer_factor_node,
                       bNodeSocket *&layer_factor_socket);

  PaintLayersTreeBuilder &outer_;
  float location_x_;
  float location_y_;
};

}  // namespace bke::paint_layers
}  // namespace blender
