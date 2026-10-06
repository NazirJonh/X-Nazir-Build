/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * A row's mask stack packed into one nested group.
 *
 * Why a separate module: the mask chain used to live flat in the row's own group, so every
 * Fill/Image item's grey chain, Mix and factor Multiply crowded the Mask/Factor columns. The
 * shader itself must not change here, only the packing, so this module owns the nested group's
 * lifetime (find-or-create by row marker plus subkind, always rebuilt together with the row) and
 * the row builder only asks it for the group to build into.
 */

#pragma once

#include "BLI_string_ref.hh"

namespace blender {

struct bNode;
struct bNodeTree;
struct bNodeTreeInterfaceSocket;
struct MaterialPaintLayer;
struct PaintLayersBuildContext;

namespace bke::paint_layers {

/* One row's packed mask stack: its own tree plus the instance standing for it in the row group. */
struct SubGroup {
  bNodeTree *tree = nullptr;
  bNode *instance = nullptr;
  bNode *group_input = nullptr;
  bNode *group_output = nullptr;
  /* Why a flag: the row owns one subgroup tree but one instance per channel it takes part in, so
   * only the first instance's channel builds the interface and the nodes. Every later channel
   * (`built == false`) reuses the tree and only binds its own instance's sockets. */
  bool built = false;
};

/* Find or create the `.PL Mask <row>` group of \a row in \a parent_tree (the row's own group) for
 * \a channel. A previous tree with the same row marker and subkind is reused without any topology
 * hash of its own: it is always cleared and rebuilt together with the row. Every channel gets its
 * own instance of the one tree, told apart by `custom1` (the channel number); the label
 * `Mask ch<N>` is only for people. Only `kind == "Mask"` exists; any other kind answers empty. */
SubGroup subgroup_ensure(const PaintLayersBuildContext &ctx,
                         const MaterialPaintLayer &row,
                         const char *kind,
                         bNodeTree &parent_tree,
                         int channel);

/* Refresh the Group Input/Output and parent instance sockets of \a sub after its interface grew. */
void subgroup_refresh(SubGroup &sub);

/* Add an input or output socket to \a sub's interface and grow its nodes to match. */
bNodeTreeInterfaceSocket *subgroup_add_input(SubGroup &sub,
                                             const char *base,
                                             StringRef socket_type);
bNodeTreeInterfaceSocket *subgroup_add_output(SubGroup &sub,
                                              const char *base,
                                              StringRef socket_type);

}  // namespace bke::paint_layers
}  // namespace blender
