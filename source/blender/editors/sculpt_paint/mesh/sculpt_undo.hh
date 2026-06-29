/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <cstdint>
#include <string>

#include "BLI_function_ref.hh"
#include "BLI_index_mask_fwd.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

struct SculptLayer;

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

enum class Type : int8_t {
  None,
  Position,
  HideVert,
  HideFace,
  Mask,
  DyntopoBegin,
  DyntopoEnd,
  Geometry,
  FaceSet,
  Color,
  SculptLayer,
};

struct StepData;

/**
 * Store undo data of the given type for a pbvh::Tree node. This function can be called by multiple
 * threads concurrently, as long as they don't pass the same pbvh::Tree node.
 *
 * This is only possible when building an undo step, in between #push_begin and #push_end.
 */
void push_node(const Depsgraph &depsgraph,
               const Object &object,
               const bke::pbvh::Node *node,
               undo::Type type);
void push_nodes(const Depsgraph &depsgraph,
                Object &object,
                const IndexMask &node_mask,
                undo::Type type);

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

/**
 * Sculpt layers (mesh/vertex domain): store per-vertex \a deltas (new - old layer value) at the
 * given \a verts into the current undo step. Undo subtracts the delta; redo adds it. All vectors
 * are consumed (moved) into the undo step to avoid a copy. Call at stroke end, before #push_end.
 *
 * \a verts and \a deltas may carry per-node holes; \a seg_start / \a seg_count list each node's
 * valid range so restore can skip the holes without a compact pass at stroke end. Pass empty
 * segment vectors when the arrays are already tightly packed.
 */
void store_active_sculpt_layer_verts(Object &object,
                                     Vector<int> &&verts,
                                     Vector<float3> &&deltas,
                                     Vector<int> &&seg_start,
                                     Vector<int> &&seg_count);

/**
 * Sculpt layers (multires/grid domain): store the explicit per-element tangent-displacement
 * \a deltas (new - old layer value) for the given \a grids into the current undo step. The delta
 * layout is `grids.size() * grid_area` values in the order of \a grids (see
 * #multiresModifier_reshapeFromCCG_into_sculpt_layer). Undo subtracts the delta from the layer
 * data, redo adds it; the CCG positions themselves are re-evaluated from base + layers. All
 * vectors are consumed (moved). Call at stroke end, before #push_end.
 */
void store_active_sculpt_layer_grids(Object &object, Vector<int> &&grids, Vector<float3> &&deltas);

/**
 * Sculpt layer operators: push a #Type::SculptLayer undo step that captures the layer's metadata
 * (influence + flag) for reversible influence/visibility changes. Call between #push_begin and
 * #push_end; the step's type is set to #Type::SculptLayer automatically.
 */
void push_sculpt_layer_metadata(Object &object, const SculptLayer &layer);

/**
 * Sculpt layer operators: push a #Type::SculptLayer undo step that captures the layer's metadata
 * AND a full snapshot of its displacement data. Use for data-mutating ops (clear, invert) where
 * position data changes. Call between #push_begin and #push_end.
 */
void push_sculpt_layer_data(Object &object, const SculptLayer &layer);

/**
 * Solo Base toggle: push a #Type::SculptLayer undo step that captures the pre-change flags of
 * every layer the toggle modifies (\a uids and \a flags run in parallel and are consumed).
 * Undo/redo swaps the stored flags with the live ones. Call between #push_begin and #push_end,
 * before the flags are modified.
 */
void push_sculpt_layer_solo(Object &object, Vector<int> &&uids, Vector<int> &&flags);

/**
 * Full snapshot of a sculpt layer for undoing layer-list changes (add / remove / duplicate /
 * merge / bake). The displacement buffer ownership moves between the undo step and the mesh's
 * layer list, so no data copy is made. Move-only.
 */
struct SculptLayerUndoPayload {
  std::string name;
  float influence = 1.0f;
  int flag = 0;
  int totelem = 0;
  int uid = 0;
  short domain = 0;
  short level = 0;
  /** Position in #Mesh::sculpt_layers at capture time (re-insertion point). */
  int index = 0;
  /** Owned while stored in the undo step; freed with the step. */
  void *data = nullptr;

  SculptLayerUndoPayload() = default;
  SculptLayerUndoPayload(const SculptLayerUndoPayload &) = delete;
  SculptLayerUndoPayload &operator=(const SculptLayerUndoPayload &) = delete;
  SculptLayerUndoPayload(SculptLayerUndoPayload &&other) noexcept;
  SculptLayerUndoPayload &operator=(SculptLayerUndoPayload &&other) noexcept;
  ~SculptLayerUndoPayload();
};

/** Capture \a layer into a payload, transferring ownership of its data buffer. The layer struct
 * itself is left in the mesh list untouched (the caller removes it). */
SculptLayerUndoPayload sculpt_layer_payload_capture(Mesh &mesh, SculptLayer &layer);

/**
 * Sculpt layer operators: record a layer-list change into the current #Type::SculptLayer undo
 * step. \a removed holds payloads of layers the operator removes from the list (remove, merge,
 * bake); \a added_uids lists uids of layers the operator adds (add, duplicate). On undo, removed
 * payloads are re-inserted and added layers are extracted into the step (and vice versa on redo).
 * When \a is_bake is true, each payload's contribution is also subtracted from / added back to
 * the multires base displacement (see #BKE_multires_sculpt_layer_apply_to_mdisps).
 * Call between #push_begin and #push_end.
 */
void push_sculpt_layer_list_change(Object &object,
                                   Vector<SculptLayerUndoPayload> &&removed,
                                   Vector<int> &&added_uids,
                                   bool is_bake);

/**
 * Sculpt layer operators: record a layer reorder (move up/down) into the current
 * #Type::SculptLayer undo step.
 */
void push_sculpt_layer_move(Object &object, const SculptLayer &layer, int index_from, int index_to);

/**
 * Sculpt layers (mesh/vertex domain): iterate the unique vertices recorded into the in-progress
 * Position undo step, passing each touched node's vertex indices together with their pre-stroke
 * positions. Returns false when there is no suitable in-progress mesh Position step (e.g. after
 * #push_end, or for a multires step). Must be called before #push_end.
 */
bool foreach_recorded_position_mesh(
    FunctionRef<void(Span<int> verts, Span<float3> orig_positions)> fn);

/**
 * Sculpt layers (multires/grid domain): iterate the grid indices recorded into the in-progress
 * Position undo step, per touched node. Returns false when there is no suitable in-progress grids
 * Position step. Must be called before #push_end.
 */
bool foreach_recorded_grids(FunctionRef<void(Span<int> grids)> fn);

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
