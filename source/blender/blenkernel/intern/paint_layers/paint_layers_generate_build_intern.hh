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
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_uuid_types.h"

#include "paint_layers_generate_layout.hh"

#include <memory>

namespace blender {

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

/** One built row of one channel: its instance in the parent and the sockets the parent chains. */
struct RowResult {
  bool valid = false;
  bNode *instance = nullptr;
  /** The instance socket the chain below feeds. */
  bNodeSocket *below = nullptr;
  /** The instance socket carrying the row laid over what is below. */
  bNodeSocket *result = nullptr;
};

/** A node and one of its output sockets: what the next stage of a chain reads from. */
struct NodeOutput {
  bNode *node = nullptr;
  bNodeSocket *socket = nullptr;
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
      : ma_(ma), tree_(tree), ctx_(ctx)
  {
  }

  void build();

 private:
  /** The group of \a layer, created on first use: its tree, its instance and its value inputs. */
  LayerGroup *layer_group_ensure(const MaterialPaintLayer &layer, float location_x, float location_y);

  /** The value inputs of \a layer: one opacity per wired channel the row takes part in, and the
   * constant of every map-less channel. Their values are written where they are created. */
  void create_value_inputs(LayerGroup &group, const MaterialPaintLayer &layer);

  /** One row of one channel: its group contents, and the instance sockets the root chains. */
  RowResult build_row(const MaterialPaintLayer &layer, LayerGroup &group, int channel);

  /**
   * The blend node of a row in \a channel: a Normal Combine instance for Normal when the shared
   * group exists, a Mix otherwise. Empty when its sockets cannot be found.
   */
  BlendNode blend_node_add(bNodeTree &tree, int channel);

  /** `a * b` as a Math node in \a tree; empty when it cannot be built. */
  NodeOutput multiply_nodes_add(bNodeTree &tree, NodeOutput a, NodeOutput b);

  /** The interface and the group output node; false when the output node cannot be created. */
  bool result_interface_create();

  /** The RGB constant a channel's chain starts from, at \a location_y. */
  NodeOutput channel_bottom_add(int channel, float location_y);

  /** The rows of \a channel chained over \a below; \a location_x advances past what was built. */
  NodeOutput channel_chain_build(int channel, NodeOutput below, float location_y, float &location_x);

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
  Vector<int> wired_channels_;

  /** A row's opacity input per channel, keyed by the row. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> opacity_inputs_;
  /** The constant input of a row's map-less channel, keyed by the row and the channel. */
  Map<const MaterialPaintLayer *, Map<int, bNodeTreeInterfaceSocket *>> fill_inputs_;
  Map<int, bNodeTreeInterfaceSocket *> result_outputs_;
  bNode *group_output_ = nullptr;
  /** Node positions of this build; a fresh builder starts from row zero. */
  layout::LayoutGrid grid_;
  Map<const MaterialPaintLayer *, LayerGroup *> layer_groups_;
  Vector<std::unique_ptr<LayerGroup>> layer_group_storage_;
};

/* -------------------------------------------------------------------- */
/** \name Layer-group interface helpers
 *
 * Stateless helpers over one #LayerGroup (and the interface it owns).
 * \{ */

/** Refresh the Group Input/Output and parent instance sockets of \a group after its interface grew. */
void refresh_layer_group(LayerGroup &group);

/** Add an interface socket of \a group and grow its three nodes to match. */
bNodeTreeInterfaceSocket *layer_group_add_socket(LayerGroup &group,
                                                 const char *base,
                                                 StringRef socket_type,
                                                 NodeTreeInterfaceSocketFlag flag);

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
