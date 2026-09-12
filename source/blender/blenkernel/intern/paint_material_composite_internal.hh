/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"

namespace blender {

struct bNodeSocket;

/**
 * Interface-socket identifiers of the Normal Combine group, fixed by the creation order in
 * #BKE_paint_material_normal_combine_group_ensure. A group instance inherits these `Socket_N`
 * identifiers verbatim, while its socket *names* ("A", "Result", ...) are display strings and are
 * translated when the "New Data" translation preference is on. #node_find_socket matches
 * identifiers, so a reader of a combine-group instance must ask for these, not for the names.
 */
constexpr const char *NORMAL_COMBINE_ID_A = "Socket_0";
constexpr const char *NORMAL_COMBINE_ID_B = "Socket_1";
constexpr const char *NORMAL_COMBINE_ID_FACTOR = "Socket_2";
constexpr const char *NORMAL_COMBINE_ID_RESULT = "Socket_3";

/** The inputs of a Mix operation. Shared by the evaluator and the UI stack model. */
struct CompositeMixNode {
  const bNodeSocket *factor = nullptr;
  const bNodeSocket *bottom = nullptr;
  const bNodeSocket *top = nullptr;
  CompositeBlend blend = CompositeBlend::Mix;
  /**
   * Whether #blend is the node's own mode rather than a stand-in.
   *
   * Screen, Difference, Hue and the rest have no byte blend function here, so the preview cannot
   * reproduce them and the channel goes to the bake instead. That is a limit of the *evaluator*:
   * such a node is still a layer, it still has a map, a factor and rows below it, and the model and
   * the edit operations must keep reading it as one. Only the evaluator may refuse it.
   */
  bool blend_supported = true;
  /**
   * When #factor is linked through a Math node in Multiply mode that combines a per-pixel
   * coverage source with a plain constant: that constant -- the layer's own editable opacity,
   * kept alongside coverage rather than replaced by it. Null when #factor is a bare constant
   * already, or linked to something this shape does not recognize.
   */
  const bNodeSocket *factor_opacity = nullptr;
  /** The Multiply's other, linked input -- what #factor_opacity multiplies against. Only set
   * alongside #factor_opacity. */
  const bNodeSocket *factor_coverage = nullptr;
};

struct Material;

/**
 * The socket a channel's layer chain ends at: the Principled input, or -- for Normal, whose
 * Principled input carries an already transformed vector -- the Color input of the Normal Map node
 * one step earlier.
 *
 * Shared so that the reader and the graph editor cannot disagree about where a chain starts.
 */
const bNodeSocket *paint_material_channel_socket_find(const Material &ma, int channel);

/** A socket's own default as a colour: scalars ride every component, like fills do. */
void paint_layer_socket_default_color(const bNodeSocket &socket, float r_color[4]);

bool composite_mix_node_read(const bNode &node, CompositeMixNode &r_mix);

/**
 * The Image Texture a layer's map input reads, when a single link from one is all that feeds it.
 * The topology cache of the node's tree must be current.
 */
const bNode *composite_mix_map_node(const CompositeMixNode &mix);

/**
 * Whether the row adds nothing to this channel by construction: the per-channel Multiply shape
 * with its coverage input unlinked (invariant I1), whatever feeds the map input.
 */
bool composite_mix_coverage_off(const CompositeMixNode &mix);

/**
 * The row's state in this channel, read from the graph alone (invariant I2). False when the Mix is
 * not the per-channel shape -- no coverage/opacity Multiply, or a map input fed by something other
 * than one Image Texture -- which is none of the three states and is left alone by every edit.
 *
 * The one place the rule lives: the evaluator, the stack model and the graph editor all read a
 * row's channel through here, so they cannot disagree.
 */
bool composite_mix_channel_state_get(const CompositeMixNode &mix,
                                     PaintMaterialLayerChannelState &r_state);
bool composite_image_from_socket(const bNodeSocket &socket,
                                 Image *&r_image,
                                 const ImageUser *&r_iuser,
                                 bool *r_from_alpha = nullptr);
const bNode *composite_source_node_shallow(const bNodeSocket &socket);

}  // namespace blender
