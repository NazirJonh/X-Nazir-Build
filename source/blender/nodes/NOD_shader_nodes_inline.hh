/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "DNA_node_types.h"

namespace blender {

struct bNodeTree;
struct bNode;
struct bNodeSocket;

namespace nodes {

struct InlineShaderNodeTreeParams {
  /**
   * Disable loop unrolling and keep Repeat Zone nodes in the tree.
   * (For engines with native support for Repeat Zones)
   *
   * Some Repeat Zones may still be unrolled (eg. if they have Closure or Bundle Zone Items).
   */
  bool allow_preserving_repeat_zones = false;

  /* Allow processing only the outputs relevant to specific engines. */
  NodeShaderOutputTarget target_engine_ = SHD_OUTPUT_ALL;

  struct ErrorMessage {
    /* In theory, more contextual information could be added here like the entire context path to
     * that node. In practice, we can't report errors with that level of detail in shader nodes
     * yet. */
    const bNode *node;
    std::string message;
  };
  Vector<ErrorMessage> r_error_messages;

  /**
   * Optional: when set, receives for every constant written into the inlined tree the socket whose
   * stored value supplied it. A GPU material built for an engine that keeps socket values in a
   * uniform buffer can then re-read those values on an edit instead of rebuilding the material.
   *
   * A socket without an entry either has no value of its own (a link) or its value is the result of
   * a computation, which a value edit has to rebuild anyway.
   */
  Map<const bNodeSocket *, const bNodeSocket *> *value_origin = nullptr;

  /**
   * Set when a node was constant-folded, i.e. the tree holds a value computed from other values
   * instead of copied from a socket. #value_origin cannot name a source for such a value, so a
   * reader that re-reads socket values must not trust the rest of the map either.
   */
  bool r_had_folded_values = false;

  /**
   * Used with #value_origin: a node that reads a value taken from a socket of one of these trees is
   * copied to the result instead of being constant-folded (when it has a GPU function), so the value
   * keeps its source socket and an edit can be applied without rebuilding.
   */
  Vector<const bNodeTree *> unfold_origin_trees;

  /**
   * Filled when #value_origin is set: the source socket of every value a constant-folded node read.
   * A folded result is only stale after an edit if one of these sockets is the one that was edited,
   * so a reader can keep tracking values when none of them is.
   */
  Vector<const bNodeSocket *> r_folded_origins;
};

bool inline_shader_node_tree(const bNodeTree &src_tree,
                             bNodeTree &dst_tree,
                             InlineShaderNodeTreeParams &params);

}  // namespace nodes
}  // namespace blender
