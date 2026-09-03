/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_paint_material_composite.hh"

namespace blender {

struct bNodeSocket;

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

bool composite_mix_node_read(const bNode &node, CompositeMixNode &r_mix);
bool composite_image_from_socket(const bNodeSocket &socket,
                                 Image *&r_image,
                                 const ImageUser *&r_iuser,
                                 bool *r_from_alpha = nullptr);
const bNode *composite_source_node_shallow(const bNodeSocket &socket);

}  // namespace blender
