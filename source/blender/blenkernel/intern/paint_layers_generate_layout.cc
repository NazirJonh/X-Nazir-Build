/* SPDX-FileCopyrightText: 2026 Blender Authors
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
#include "BLI_vector.hh"

#include "DNA_node_types.h"

namespace blender {
namespace bke::paint_layers::layout {

namespace {

/* Why thread_local: a bake worker builds its own Main on its own thread, so two concurrent builds
 * must not share the lane state; it is per-thread and reset at each build pass. */
struct LaneState {
  int lane = 0;
  /* Lane -> first absolute row of the lane; computed from the previous lane's tallest row plus the
   * gap when the lane is first entered. */
  Vector<int> base;
  /* Lane -> highest row index placed in it, tracked as `at` is called. */
  Vector<int> max_row;
};

thread_local LaneState lane_state;

/* Why two rows: neighbours are read as 140x160 boxes one kRowPitch apart, so two spare rows always
 * leave one channel's block clear of the next channel's. */
constexpr int kLaneGapRows = 2;

/* NOTE: a lane's base is fixed when it is first entered, from the previous lane's height at that
 * moment. This only holds while the build loop finishes lane N-1 for every group before entering
 * lane N (channels outer, groups inner); inverting that loop would let lanes overlap. */
void ensure_lane(const int lane)
{
  LaneState &state = lane_state;
  while (state.base.size() <= lane) {
    const int previous = int(state.base.size()) - 1;
    const int start = (previous < 0) ?
                          0 :
                          state.base[previous] + state.max_row[previous] + kLaneGapRows;
    state.base.append(start);
    state.max_row.append(0);
  }
}

}  // namespace

void reset_lanes()
{
  lane_state = LaneState{};
}

ChannelLaneScope::ChannelLaneScope(const int lane) : previous_lane_(lane_state.lane)
{
  lane_state.lane = lane;
  ensure_lane(lane);
}

ChannelLaneScope::~ChannelLaneScope()
{
  lane_state.lane = previous_lane_;
}

Cursor at(Column column, int row)
{
  /* Why origin-relative: nodes inside a row group are counted from the group origin, never from the
   * root cursor, so two mask items cannot share a spot. The lane base keeps one channel's whole
   * chain clear of the next channel's without a fixed per-lane height. */
  LaneState &state = lane_state;
  ensure_lane(state.lane);
  if (row > state.max_row[state.lane]) {
    state.max_row[state.lane] = row;
  }
  Cursor cursor;
  cursor.x = float(int(column)) * kColumnWidth;
  cursor.y = -float(state.base[state.lane] + row) * kRowPitch;
  return cursor;
}

void place(bNode &node, Column column, int row, float dx, float dy)
{
  /* Why a helper: every builder spells the same grid, so a pitch change stays
   * in one place and the shader order is untouched. */
  const Cursor cursor = at(column, row);
  {
    node.location[0] = cursor.x + dx;
  }
  {
    node.location[1] = cursor.y + dy;
  }
}

void place_extra(bNode &node)
{
  LaneState &state = lane_state;
  ensure_lane(state.lane);
  /* Why next free row: a fresh row per call keeps every ad-hoc node of the lane in its own cell,
   * whatever block built it, and the Extra column is clear of the named ones. */
  const Cursor cursor = at(Column::Extra, state.max_row[state.lane] + 1);
  node.location[0] = cursor.x;
  node.location[1] = cursor.y;
}

void label(bNode &node, const char *text)
{
  /* Why a helper: labels are UI only and never enter the topology hash, so
   * they cannot move the shader; the copy keeps the DNA char buffer safe. */
  if (text != nullptr) {
    STRNCPY_UTF8(node.label, text);
  }
}

bNode *frame_add(bNodeTree &tree, const char *label_text, Span<bNode *> children)
{
  /* Why a static frame: NODE_FRAME carries no sockets, so parenting changes
   * only the editor packing and never the shader links. */
  if (children.is_empty()) {
    return nullptr;
  }
  bNode *frame = bke::node_add_static_node(nullptr, tree, NODE_FRAME);
  if (frame == nullptr) {
    return nullptr;
  }
  /* Why origin: children already sit on the grid, and node_attach_node keeps
   * their stored locations, so the frame must stay at (0, 0) for local spots
   * to read as global. */
  frame->location[0] = 0.0f;
  frame->location[1] = 0.0f;
  if (NodeFrame *data = static_cast<NodeFrame *>(frame->storage)) {
    /* Why shrink and 20: the same defaults node_frame_init gives every new
     * frame (node_common.cc), so generated frames pack like hand-made ones. */
    data->flag |= NODE_FRAME_SHRINK;
    data->label_size = 20;
  }
  label(*frame, label_text);
  /* Why after place: the children already sit on the grid, and the frame
   * itself stays at the origin, so their local spots read as global. */
  for (bNode *child : children) {
    if (child != nullptr && child != frame) {
      bke::node_attach_node(tree, *child, *frame);
    }
  }
  return frame;
}

}  // namespace bke::paint_layers::layout
}  // namespace blender
