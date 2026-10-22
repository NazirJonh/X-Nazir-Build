/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Deterministic layout of the paint-layer row group in the Shader Editor.
 *
 * Why a separate module: the builders would otherwise derive every position from a running
 * cursor, so nodes of one row land on top of each other. The shader itself is not affected by
 * positions, labels, frames or packing, so this module owns the grid and the builders only ask it
 * where a node goes.
 */

#pragma once

#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct bNode;
struct bNodeTree;

namespace bke::paint_layers::layout {

/* Why fixed steps: the overlap test assumes a node box of 140x160, so a column pitch of 260 and a
 * row pitch of 220 always separate neighbours. */
inline constexpr float kColumnWidth = 260.0f;
inline constexpr float kRowPitch = 220.0f;

/**
 * Bump when the node layout of a generated group changes, so the topology hash of every row moves
 * and every kept group is rebuilt once on the next regeneration instead of keeping its old grid.
 */
inline constexpr int kLayoutVersion = 8;

enum class Column : int {
  Inputs = 0,
  Source,
  Content,
  Mask,
  Factor,
  Output,
  /* Why extra: a node the builder never gave a named slot (a combine instance, a fallback Mix)
   * still must land on the grid, in a column clear of every named one. */
  Extra,
};

struct Cursor {
  float x = 0.0f;
  float y = 0.0f;
};

/**
 * The grid one build lays its nodes on: which cell a node goes in, per channel lane.
 *
 * The state belongs to the build that owns the grid, so two builds never share positions and a
 * fresh build starts from row zero without any reset. A channel's lane is entered through
 * #LaneScope; the lane's base row is placed just past the tallest row the previous lane used, which
 * only holds while a build finishes lane N-1 for every group before entering lane N.
 */
class LayoutGrid {
 public:
  /** The cell of (\a column, \a row) in the current lane, claiming the row for the lane height. */
  Cursor at(Column column, int row);
  void place(bNode &node, Column column, int row, float dx = 0.0f, float dy = 0.0f);
  /**
   * Place \a node in the current lane's next free Extra-column cell: a builder that has no named
   * slot for a node still cannot drop it onto a laid-out one.
   */
  void place_extra(bNode &node);

  /** A channel's lane while alive; restores the previous one on destruction. */
  class LaneScope {
   public:
    LaneScope(LayoutGrid &grid, int lane);
    ~LaneScope();
    LaneScope(const LaneScope &) = delete;
    LaneScope &operator=(const LaneScope &) = delete;

   private:
    LayoutGrid &grid_;
    int previous_lane_;
  };

 private:
  /** Make sure \a lane has a base row, computed from the previous lane's tallest row. */
  void lane_ensure(int lane);

  int lane_ = 0;
  /** Lane -> first absolute row of the lane. */
  Vector<int> base_;
  /** Lane -> highest row index placed in it. */
  Vector<int> max_row_;
};

void label(bNode &node, const char *text);
/* A NODE_FRAME labelled \a label_text parenting \a children, or null. The frame stays at the
 * tree origin, so the children's grid positions read as global without any shift. */
bNode *frame_add(bNodeTree &tree, const char *label_text, Span<bNode *> children);

}  // namespace bke::paint_layers::layout
}  // namespace blender
