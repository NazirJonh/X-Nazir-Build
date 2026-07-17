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

/* Rather than a forward declaration: #SculptLayerUndoPayload stores an #eSculptLayerTreeNodeType and
 * tests it inline, which needs the enumerators, not just the type's name. The sculpt layer node
 * types themselves live in this header too, so declaring them separately would only add a second
 * spelling that has to agree with the first. */
#include "DNA_mesh_types.h"

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
 * Sculpt layer operators: push a #Type::SculptLayer undo step that captures \a node's metadata
 * (flag + name, plus #SculptLayer::influence when it is a layer) for reversible
 * influence/visibility/rename changes. Capture before the change: undo/redo swaps the stored
 * metadata with the live one. Call between #push_begin and #push_end; the step's type is set to
 * #Type::SculptLayer automatically.
 *
 * One entry point for both node kinds. The folder counterpart used to be a separate push because
 * "the uid resolves in a different list"; #bke::sculpt_layers::node_unique_uid now hands out uids
 * from a single counter spanning both kinds, so a uid names exactly one node and the restore looks
 * it up once and swaps whatever that node has.
 */
void push_sculpt_layer_metadata(Object &object, const SculptLayerTreeNode &node);

/**
 * Sculpt layer operators: push a #Type::SculptLayer undo step that captures the layer's metadata
 * AND a full snapshot of its displacement data. Use for data-mutating ops (clear, invert) where
 * position data changes. Call between #push_begin and #push_end.
 */
void push_sculpt_layer_data(Object &object, const SculptLayer &layer);

/**
 * Push a #Type::SculptLayer undo step that captures the pre-change #SculptLayerTreeNode::flag of a
 * set of layers (\a uids and \a flags run in parallel and are consumed). Undo/redo swaps the
 * stored flags with the live ones, which is its own inverse.
 *
 * A plain batch flag swap, not tied to any one feature: the Solo Base toggle uses it to hide every
 * enabled layer at once, and the folder cascade uses it to record the #SCULPT_LAYER_GROUP_HIDDEN
 * bits #bke::sculpt_layers::resync_group_hidden recomputed. Capture the flags before they are
 * modified, and call between #push_begin and #push_end.
 */
void push_sculpt_layer_flags_batch(Object &object, Vector<int> &&uids, Vector<int> &&flags);

/**
 * Full snapshot of one node of the sculpt layer tree — of *either* kind — for undoing tree changes
 * (layer add / remove / duplicate / merge / bake, folder create / disband).
 *
 * One payload rather than a layer one and a folder one: a payload names its slot by the uid of the
 * sibling it followed, and #bke::sculpt_layers::node_unique_uid now hands out uids from a single
 * counter spanning both kinds. A uid therefore names exactly one node, and an anchor no longer has
 * to say which list to resolve it in — which is what the two payloads existed for. A folder is an
 * ordinary sibling of a layer in #SculptLayerGroup::children and can be one's anchor.
 *
 * #type says which kind the payload holds and is the only thing that may be read unconditionally
 * besides #name / #flag / #uid / #parent_uid / #prev_uid. The layer-only fields (#influence,
 * #totelem, #domain, #level and the #data buffer) are meaningful only when #is_layer is true; every
 * path that fills or reads them goes through that test, so a folder payload never acquires a data
 * buffer and can never reach the free in the destructor.
 *
 * The displacement buffer ownership moves between the undo step and the tree, so no data copy is
 * made. Move-only.
 */
struct SculptLayerUndoPayload {
  /** #eSculptLayerTreeNodeType, mirroring #SculptLayerTreeNode::type. */
  int8_t type = SCULPT_LAYER_TREE_NODE_TYPE_LAYER;
  std::string name;
  int flag = 0;
  int uid = 0;
  /**
   * The folder the node sat in, as its uid (0 when it sat at the root — the root group holds uid 0,
   * so this reads back through #bke::sculpt_layers::node_find_by_uid without a special case).
   *
   * Must be carried like any other field: without it, re-insertion puts the node back at the root
   * and undo silently re-parents it, while a layer's restored #flag may still carry
   * #SCULPT_LAYER_GROUP_HIDDEN for a folder it no longer belongs to.
   */
  int parent_uid = 0;
  /**
   * Where the node sat among its siblings at capture time, recorded as the uid of the sibling it
   * followed (0 when it was the head). Re-insertion goes after that sibling.
   *
   * A neighbour rather than a position, because a position only names the same slot for as long as
   * nothing else in the folder moves. When several nodes are captured together, each one's anchor
   * may be another captured node — of either kind, since a folder and a layer are siblings in one
   * list — and re-inserting them in capture order then rebuilds the original sequence, since a
   * node's anchor is always restored before it is.
   */
  int prev_uid = 0;

  /* Layer-only, see #is_layer. Left at their defaults by a folder capture. */
  float influence = 1.0f;
  int totelem = 0;
  short domain = 0;
  short level = 0;
  /** Owned while stored in the undo step; freed with the step. Always null for a folder payload. */
  void *data = nullptr;

  /** Whether the layer-only fields above carry anything, i.e. whether #data may be non-null. */
  bool is_layer() const
  {
    return type == SCULPT_LAYER_TREE_NODE_TYPE_LAYER;
  }

  SculptLayerUndoPayload() = default;
  SculptLayerUndoPayload(const SculptLayerUndoPayload &) = delete;
  SculptLayerUndoPayload &operator=(const SculptLayerUndoPayload &) = delete;
  SculptLayerUndoPayload(SculptLayerUndoPayload &&other) noexcept;
  SculptLayerUndoPayload &operator=(SculptLayerUndoPayload &&other) noexcept;
  ~SculptLayerUndoPayload();
};

/**
 * Capture \a node into a payload. For a layer this transfers ownership of its data buffer; a folder
 * has none to transfer. The node struct itself is left linked in the tree untouched (the caller
 * removes it, or gives the layer a new buffer).
 *
 * Takes the shared #SculptLayerTreeNode rather than either concrete type, so that one call site
 * spelling covers both kinds; pass `layer.base` or `group.base`.
 */
SculptLayerUndoPayload sculpt_layer_payload_capture(Mesh &mesh, SculptLayerTreeNode &node);

/**
 * Sculpt layer operators: push a #Type::SculptLayer undo step for layers whose data buffer was
 * *replaced* rather than edited in place, i.e. a resize (#SCULPT_OT_layer_validate repairing stale
 * layers). Unlike #push_sculpt_layer_data — which snapshots the buffer and can only restore it when
 * the size still matches — each payload carries its own `totelem`, so the two states may differ in
 * size. The layers stay in the list and keep their uid; only the buffers are exchanged.
 *
 * Each payload must come from #sculpt_layer_payload_capture (which takes the *old* buffer off the
 * layer) and the caller must then install the new buffer. Consumed. Call between #push_begin and
 * #push_end.
 */
void push_sculpt_layer_data_resize(Object &object, Vector<SculptLayerUndoPayload> &&resized);

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
 * Bake on a mesh with relative shape keys: record the key block the bake created (identified by
 * #KeyBlock::uid, see #bke::sculpt_layers::bake_vert_layers_into_new_shape_key) so undo detaches it
 * from the mesh (the step then owns it) and redo links it back. Call between #push_begin and
 * #push_end, after #push_sculpt_layer_list_change.
 */
void push_sculpt_layer_bake_shape_key(Object &object, int key_uid);

/**
 * Bake on a mesh with NO shape keys yet: record that this step's operator created the mesh's
 * #Key from scratch (a #KeyBlock::name "Basis" block plus the combined-layer block), rather than
 * adding one block to an already-existing key like #push_sculpt_layer_bake_shape_key does. Undo
 * fully tears the #Key back down (#BKE_object_shapekey_free) and redo fully rebuilds it
 * (#bke::sculpt_layers::bake_vert_layers_into_new_shape_key), rather than preserving the same
 * #Key / #KeyBlock structs across the undo boundary. \a pre_bake_shapenr is #Object::shapenr
 * from before the operator ran, restored verbatim on undo. Call between #push_begin and
 * #push_end, after #push_sculpt_layer_list_change.
 */
void push_sculpt_layer_bake_to_shape_key(Object &object, short pre_bake_shapenr);

/**
 * One node's move within a #push_sculpt_layer_reparent batch. Covers both halves of a move at once,
 * because they are the same thing recorded twice otherwise: where the node sits among its siblings
 * (\a prev_from / \a prev_to) and which folder contains it (\a group_from / \a group_to). A plain
 * reorder simply leaves the group fields equal.
 *
 * The entry carries no "which kind of node is this" flag: #bke::sculpt_layers::node_unique_uid hands
 * out uids from a single counter spanning layers and folders, so #uid names exactly one node and the
 * restore resolves it — and both anchors — with one #bke::sculpt_layers::node_find_by_uid.
 */
struct ReparentMove {
  int uid = 0;
  /** Sibling anchor before / after the move, among the children of #group_from / #group_to
   * respectively (0 = head). A neighbour rather than an index, and it may name a node of either
   * kind — see #SculptLayerUndoPayload::prev_uid. */
  int prev_from = 0;
  int prev_to = 0;
  /** Containing folder's #SculptLayerTreeNode::uid before / after the move (0 = the root folder). */
  int group_from = 0;
  int group_to = 0;
};

/**
 * Sculpt layer operators: record one or more node moves (reorder, reparent, or both at once) into
 * the current #Type::SculptLayer undo step as a single batch — one undo step moves every listed
 * node. \a moves must be in the order the nodes were captured (their relative order before the
 * move); #restore_list re-applies them in that same order on both undo and redo, since each entry's
 * anchor may itself be another entry in the same batch (mirrors #SculptLayerUndoPayload::prev_uid).
 *
 * That reasoning now spans both kinds: a folder and a layer are siblings in one
 * #SculptLayerGroup::children list, so a *folder* can be the anchor a layer's \a prev_from /
 * \a prev_to names, and vice versa. One uid space is what makes that unambiguous; the ordering rule
 * itself is unchanged, it simply has to hold over a batch mixing the two.
 *
 * The batch only restores positions and parents. A reparent that changes what is visible must
 * additionally record the recomputed flags (#push_sculpt_layer_flags_batch), since
 * #SCULPT_LAYER_GROUP_HIDDEN is derived state that this batch does not recompute.
 */
void push_sculpt_layer_reparent(Object &object, Vector<ReparentMove> &&moves);

/**
 * Sculpt layer operators: record an active-layer selection change (pure UI state) into the
 * current #Type::SculptLayer undo step. Undo restores \a uid_from, redo \a uid_to; 0 means no
 * active layer. Kept as a sculpt step (rather than a global undo push) so it composes with stroke
 * SCULPT steps.
 */
void push_sculpt_layer_active(Object &object, int uid_from, int uid_to);

/**
 * Sculpt layer operators: record a folder-tree change into the current #Type::SculptLayer undo step.
 * \a removed holds payloads of folders the operator removes, \a added_uids the uids of folders it
 * adds. On undo, removed payloads are re-inserted and added folders are extracted into the step (and
 * vice versa on redo). Call between #push_begin and #push_end.
 *
 * Folders take the same payload type as layers now, but stay a *separate* list from
 * #push_sculpt_layer_list_change's for an ordering reason the tree introduced: a node can only be
 * linked into a folder that exists, and a folder can only be freed once it is empty. #restore_list
 * therefore inserts folders before it re-applies the #push_sculpt_layer_reparent batch and extracts
 * them after it, which is exactly what makes "create a folder and move nodes into it" and "disband a
 * folder, lifting its children out" undo and redo. Merging the two lists would lose that seam.
 *
 * #push_sculpt_layer_list_change's own \a removed / \a added run *after* that seam, not inside it:
 * a step must not record a folder in \a removed / \a added here while, in the same step, a layer
 * that lives inside it sits in #push_sculpt_layer_list_change's \a removed / \a added. Doing so
 * would make #restore_list extract the folder while the layer is still one of its children,
 * hitting the same empty-folder assert this seam exists to avoid (and, without asserts, freeing
 * the folder with the layer still hanging off it). No operator does this today: folder ops only
 * ever populate this function's lists plus a reparent batch, and layer add/remove/bake only ever
 * populate #push_sculpt_layer_list_change's. Nothing in the types stops a future caller from doing
 * both at once, so it remains a rule to keep rather than a bug that shows up.
 *
 * The payload merge above also cuts the other way: a layer's uid and a folder's uid used to be
 * separate fields before one counter started naming both kinds, so a single
 * #push_sculpt_layer_metadata or #push_sculpt_layer_data step could name a layer and a folder at
 * once. Now #uid is the only slot; one such step names exactly one node. No caller has ever
 * needed both at once.
 *
 * The caller must still empty a folder itself (see #SCULPT_OT_layer_group_remove, which records the
 * lift-out as a reparent batch): what an orphaned subtree should become is the operator's decision.
 * Within one batch a folder must precede any folder nested inside it (top-down), so that insertion
 * always finds the parent already there; #restore_list extracts the batch in the reverse order so
 * that a nested folder is freed before the parent that owns it (see #SCULPT_OT_layer_group_merge,
 * which removes a whole nested subtree of folders in one step).
 */
void push_sculpt_layer_group_list_change(Object &object,
                                         Vector<SculptLayerUndoPayload> &&removed,
                                         Vector<int> &&added_uids);

/**
 * Sculpt layers (mesh/vertex domain): iterate the unique vertices recorded into the in-progress
 * Position undo step, passing each touched node's vertex indices together with their pre-stroke
 * positions. Returns false when there is no suitable in-progress mesh Position step (e.g. after
 * #push_end, or for a multires step). Must be called before #push_end.
 */
bool foreach_recorded_position_mesh(
    FunctionRef<void(Span<int> verts, Span<float3> orig_positions)> fn);

/**
 * Like #foreach_recorded_position_mesh but always yields the evaluated/display positions
 * (#Node.position) rather than preferring #orig_position (base space) when a deform is active. Used
 * by the shape-key sculpt-layer recorder, which diffs the pre-stroke evaluated positions against the
 * post-stroke #deform_cos to recover the object-space per-vertex layer delta.
 */
bool foreach_recorded_eval_position_mesh(
    FunctionRef<void(Span<int> verts, Span<float3> eval_positions)> fn);

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
