/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <cstdint>

#include "BLI_enum_flags.hh"
#include "BLI_index_mask_fwd.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Depsgraph;
struct Mesh;
struct Object;
struct Scene;
struct wmOperator;
namespace bke::pbvh {
class Node;
}

namespace ed::sculpt_paint::undo {

/* Flags describing which per-node data is stored in an undo step.
 * Multiple flags may be ORed together. */
enum class NodeDataFlag : uint8_t {
  Position = (1 << 0),
  FaceSet  = (1 << 1),
  Color    = (1 << 2),
  Mask     = (1 << 3),
  HideVert = (1 << 4),
  HideFace = (1 << 5),
};
ENUM_OPERATORS(NodeDataFlag)

/* Type of a special non-per-node undo step.
 * Mutually exclusive with NodeDataFlag steps. */
enum class Type : int8_t {
  None,
  Geometry,
  DyntopoBegin,
  DyntopoEnd,
};

struct StepData;

/* Store undo data for a single PBVH node. Call between push_begin and push_end. */
void push_node(const Depsgraph &depsgraph,
               const Object &object,
               const bke::pbvh::Node *node,
               NodeDataFlag flags);
/* Store undo data for all nodes in node_mask. Thread-safe per node. */
void push_nodes(const Depsgraph &depsgraph,
                Object &object,
                const IndexMask &node_mask,
                NodeDataFlag flags);
/* Store a special non-per-node undo step (Geometry, DyntopoBegin, DyntopoEnd). */
void push_node_special(const Depsgraph &depsgraph, Object &object, Type special_type);

/**
 * Pushes an undo step using the operator name. This is necessary for
 * redo panels to work; operators that do not support that may use
 * #push_begin_ex instead if so desired.
 */
void push_begin(const Scene &scene, Object &ob, const wmOperator *op);

/**
 * Pushes an undo step when entering Sculpt mode.
 *
 * Similar to geometry_push, this undo type does not need the PBVH to be constructed.
 */
void push_enter_sculpt_mode(const Scene &scene, Object &ob, const wmOperator *op);

/**
 * NOTE: #push_begin is preferred since `name`
 * must match operator name for redo panels to work.
 */
void push_begin_ex(const Scene &scene, Object &ob, const char *name);
void push_end(Object &ob);
void push_end_ex(Object &ob, bool use_nested_undo);

void restore_from_bmesh_enter_geometry(const StepData &step_data, Mesh &mesh);
bool has_bmesh_log_entry();

void restore_position_from_undo_step(const Depsgraph &depsgraph, Object &object);

namespace compression {

/**
 * Compress a span with ZSTD, using a prefiltering step that can improve compression speed and
 * ratios for certain data.
 */
template<typename T>
void filter_compress(const Span<T> src,
                     Vector<std::byte> &filter_buffer,
                     Vector<std::byte> &compress_buffer);

/** Decompress data compressed with #filter_compress. */
template<typename T>
void filter_decompress(const Span<std::byte> src, Vector<std::byte> &buffer, Vector<T> &dst);

}  // namespace compression

}  // namespace ed::sculpt_paint::undo

}  // namespace blender
