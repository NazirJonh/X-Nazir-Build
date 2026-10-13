/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Implementation of the deterministic row-group layout grid.
 */

#include "paint_layers_generate_layout.hh"

#include "BKE_node.hh"

#include "BLI_string.h"
#include "BLI_string_utf8.h"

#include "DNA_node_types.h"

namespace blender::bke::paint_layers::layout {

/* Why two rows: neighbours are read as 140x160 boxes one kRowPitch apart, so two spare rows always
 * leave one channel's block clear of the next channel's. */
static constexpr int kLaneGapRows = 2;

void LayoutGrid::lane_ensure(const int lane)
{
  while (base_.size() <= lane) {
    const int previous = int(base_.size()) - 1;
    const int start = (previous < 0) ? 0 : base_[previous] + max_row_[previous] + kLaneGapRows;
    base_.append(start);
    max_row_.append(0);
  }
}

LayoutGrid::LaneScope::LaneScope(LayoutGrid &grid, const int lane)
    : grid_(grid), previous_lane_(grid.lane_)
{
  grid_.lane_ = lane;
  grid_.lane_ensure(lane);
}

LayoutGrid::LaneScope::~LaneScope()
{
  grid_.lane_ = previous_lane_;
}

Cursor LayoutGrid::at(const Column column, const int row)
{
  /* Why origin-relative: nodes inside a row group are counted from the group origin, never from
   * the root cursor. The lane base keeps one channel's whole chain clear of the next channel's
   * without a fixed per-lane height. */
  this->lane_ensure(lane_);
  if (row > max_row_[lane_]) {
    max_row_[lane_] = row;
  }
  Cursor cursor;
  cursor.x = float(int(column)) * kColumnWidth;
  cursor.y = -float(base_[lane_] + row) * kRowPitch;
  return cursor;
}

void LayoutGrid::place(
    bNode &node, const Column column, const int row, const float dx, const float dy)
{
  const Cursor cursor = this->at(column, row);
  node.location[0] = cursor.x + dx;
  node.location[1] = cursor.y + dy;
}

void LayoutGrid::place_extra(bNode &node)
{
  this->lane_ensure(lane_);
  /* Why next free row: a fresh row per call keeps every ad-hoc node of the lane in its own cell,
   * and the Extra column is clear of the named ones. */
  const Cursor cursor = this->at(Column::Extra, max_row_[lane_] + 1);
  node.location[0] = cursor.x;
  node.location[1] = cursor.y;
}

void label(bNode &node, const char *text)
{
  /* Labels are UI only; the copy keeps the DNA char buffer safe. */
  if (text != nullptr) {
    STRNCPY_UTF8(node.label, text);
  }
}

bNode *frame_add(bNodeTree &tree, const char *label_text, Span<bNode *> children)
{
  /* Why a static frame: NODE_FRAME carries no sockets, so parenting changes only the editor
   * packing and never the shader links. */
  if (children.is_empty()) {
    return nullptr;
  }
  bNode *frame = bke::node_add_static_node(nullptr, tree, NODE_FRAME);
  if (frame == nullptr) {
    return nullptr;
  }
  /* Why origin: children already sit on the grid, and node_attach_node keeps their stored
   * locations, so the frame must stay at (0, 0) for local spots to read as global. */
  frame->location[0] = 0.0f;
  frame->location[1] = 0.0f;
  if (NodeFrame *data = static_cast<NodeFrame *>(frame->storage)) {
    /* The same defaults node_frame_init gives every new frame (node_common.cc), so generated
     * frames pack like hand-made ones. */
    data->flag |= NODE_FRAME_SHRINK;
    data->label_size = 20;
  }
  label(*frame, label_text);
  for (bNode *child : children) {
    if (child != nullptr && child != frame) {
      bke::node_attach_node(tree, *child, *frame);
    }
  }
  return frame;
}

}  // namespace blender::bke::paint_layers::layout
