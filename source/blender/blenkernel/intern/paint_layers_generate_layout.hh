/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Deterministic layout of the paint-layer row group in the Shader Editor.
 *
 * Why a separate module: the chain builders used to derive every intra-group
 * position from the root cursor (`location_x_` plus ad-hoc offsets such as
 * `location_x - 220`), so every mask item of one row landed on the same spot
 * and the grey-chain nodes without any location stayed at the origin. The
 * shader itself must not change here, only positions, labels, frames and
 * packing, so this module owns the grid and the builders only ask it where a
 * node goes.
 */

#pragma once

#include "BLI_span.hh"

namespace blender {

struct bNode;
struct bNodeTree;

namespace bke::paint_layers::layout {

/* Why fixed steps: the overlap test assumes a node box of 140x160, so a
 * column pitch of 260 and a row pitch of 220 always separate neighbours. */
inline constexpr float kColumnWidth = 260.0f;
inline constexpr float kRowPitch = 220.0f;
/* Why 5: one mask item owns at most five Mask-column slots (map plus the
 * four-node grey chain), while its Mix and factor nodes share the same rows
 * in the Factor column, so items packed at `index * kMaskItemRows` never meet. */
inline constexpr int kMaskItemRows = 5;
/* Why a version: unchanged groups with the same topology hash are not rebuilt,
 * so a pure layout move needs a one-time rebuild through the hash. */
inline constexpr int kLayoutVersion = 2;

enum class Column : int {
  Inputs = 0,
  Source,
  Content,
  Mask,
  Factor,
  Output,
  /* Why extra: a node the builder never gave a named slot (an ad-hoc straighten chain, a fallback
   * Mix) still must land on the grid, in a column clear of every named one. */
  Extra,
};

struct Cursor {
  float x = 0.0f;
  float y = 0.0f;
};

Cursor at(Column column, int row);
void place(bNode &node, Column column, int row, float dx = 0.0f, float dy = 0.0f);
/* Place \a node in its lane's next free Extra-column cell: a builder that has no named slot for a
 * node still cannot drop it onto a laid-out one. */
void place_extra(bNode &node);

/**
 * A channel's lane: while it is alive, every `at`/`place` shifts into lane \a lane, whose base row
 * is placed just past the tallest row the previous lane used. The scope is per-thread and restores
 * the previous lane on destruction, so nested builders (for example the shared `.PL Mask` panel,
 * which is built in lane 0 whatever channel entered it) cannot leak their shift outwards.
 */
class ChannelLaneScope {
 public:
  explicit ChannelLaneScope(int lane);
  ~ChannelLaneScope();
  ChannelLaneScope(const ChannelLaneScope &) = delete;
  ChannelLaneScope &operator=(const ChannelLaneScope &) = delete;

 private:
  int previous_lane_;
};

/* Drop the accumulated lane bases and heights so each build pass starts from row zero. */
void reset_lanes();

void label(bNode &node, const char *text);
/* A NODE_FRAME labelled \a label_text parenting \a children, or null. The frame stays at the
 * tree origin, so the children's grid positions read as global without any shift. */
bNode *frame_add(bNodeTree &tree, const char *label_text, Span<bNode *> children);

}  // namespace bke::paint_layers::layout
}  // namespace blender
