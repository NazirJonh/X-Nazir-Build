/* SPDX-FileCopyrightText: 2006 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the Sculpt Mode undo system.
 *
 * Usage Guide
 * ===========
 *
 * The sculpt undo system is a delta-based system. Each undo step stores the difference with the
 * prior one.
 *
 * To use the sculpt undo system, you must call #push_begin inside an operator exec or invoke
 * callback (#geometry_begin may be called if you wish to save a non-delta copy of the entire
 * mesh). This will initialize the sculpt undo stack and set up an undo step.
 *
 * At the end of the operator you should call #push_end.
 *
 * #push_begin and #geometry_begin both take a #wmOperatorType as an argument. There are _ex
 * versions that allow a custom name; try to avoid using them. These can break the redo panel since
 * it requires the undo push to have the same name as the calling operator.
 */
#include "sculpt_undo.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <zstd.h>

#include "CLG_log.h"

#include "BLI_array.hh"
#include "BLI_bit_group_vector.hh"
#include "BLI_compression.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_memory_counter.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "MEM_guardedalloc.h"

#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_ccg.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_scene.hh"
#include "BKE_sculpt_layers.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_undo_system.hh"

/* TODO(sergey): Ideally should be no direct call to such low level things. */
#include "BKE_subdiv_eval.hh"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_geometry.hh"
#include "ED_object.hh"
#include "ED_sculpt.hh"
#include "ED_undo.hh"

#include "bmesh.hh"
#include "mesh_brush_common.hh"
#include "paint_hide.hh"
#include "sculpt_color.hh"
#include "sculpt_dyntopo.hh"
#include "sculpt_face_set.hh"
#include "sculpt_intern.hh"

namespace blender {

// #define DEBUG_TIME

#ifdef DEBUG_TIME
#  include "BLI_timeit.hh"
#endif

static CLG_LogRef LOG = {"undo.sculpt"};

namespace ed::sculpt_paint::undo {

/* Implementation of undo system for objects in sculpt mode.
 *
 * Each undo step in sculpt mode consists of list of nodes, each node contains a flat array of data
 * related to the step type.
 *
 * Node type used for undo depends on specific operation and active sculpt mode ("regular" or
 * dynamic topology).
 *
 * Regular sculpt brushes will use Position, HideVert, HideFace, Mask, FaceSet nodes. These
 * nodes are created for every BVH node which is affected by the brush. The undo push for the node
 * happens BEFORE modifications. This makes the operation undo to work in the following way: for
 * every node in the undo step swap happens between node in the undo stack and the corresponding
 * value in the BVH. This is how redo is possible after undo.
 *
 * The COORDS, HIDDEN or MASK type of nodes contains arrays of the corresponding values.
 *
 * Operations like Symmetrize are using GEOMETRY type of nodes which pushes the entire state of the
 * mesh to the undo stack. This node contains all CustomData layers.
 *
 * The tricky aspect of this undo node type is that it stores mesh before and after modification.
 * This allows the undo system to both undo and redo the symmetrize operation within the
 * pre-modified-push of other node type behavior, but it uses more memory that it seems it should
 * be.
 *
 * The dynamic topology undo nodes are handled somewhat separately from all other ones and the idea
 * there is to store log of operations: which vertices and faces have been added or removed.
 *
 * Begin of dynamic topology sculpting mode have own node type. It contains an entire copy of mesh
 * since just enabling the dynamic topology mode already does modifications on it.
 *
 * End of dynamic topology and symmetrize in this mode are handled in a special manner as well. */

#define NO_ACTIVE_LAYER bke::AttrDomain::Auto

struct Node {
  Array<float3, 0> position;
  Array<float3, 0> orig_position;
  Array<float3, 0> normal;
  Array<float4, 0> col;
  Array<float, 0> mask;

  Array<float4, 0> loop_col;

  /* Mesh. */

  Array<int, 0> vert_indices;
  int unique_verts_num;

  /**
   * \todo Storing corners rather than faces is unnecessary.
   */
  Vector<int, 0> corner_indices;

  BitVector<0> vert_hidden;
  BitVector<0> face_hidden;

  /* Multires. */

  /** Indices of grids in the pbvh::Tree node. */
  Array<int, 0> grids;
  BitGroupVector<0> grid_hidden;

  /* Sculpt Face Sets */
  Array<int, 0> face_sets;

  Vector<int> face_indices;
};

struct SculptAttrRef {
  bke::AttrDomain domain;
  eCustomDataType type;
  char name[MAX_CUSTOMDATA_LAYER_NAME];
  bool was_set;
};

/* Storage of geometry for the undo node.
 * Is used as a storage for either original or modified geometry. */
struct NodeGeometry {
  /* Is used for sanity check, helping with ensuring that two and only two
   * geometry pushes happened in the undo stack. */
  bool is_initialized;

  bke::AttributeStorage attribute_storage;
  CustomData vert_data;
  CustomData edge_data;
  CustomData corner_data;
  CustomData face_data;
  int *face_offset_indices;
  const ImplicitSharingInfo *face_offsets_sharing_info;
  int verts_num;
  int edges_num;
  int corners_num;
  int faces_num;
};

struct Node;
struct PositionUndoStorage;

struct StepData {
 private:
  bool applied_ = true;

 public:
  /**
   * The type of data stored in this undo step. For historical reasons this is often set when the
   * first undo node is pushed.
   */
  Type type = Type::None;

  /**
   * The object this undo data belongs to. Standard Blender undo-system object reference (see
   * `editmesh_undo.cc`'s `obedit_ref` for the same pattern): #ptr is a LIVE pointer only while
   * this step is being built (`ustack->step_init`, from #push_begin to #push_end_all_ex), safe to
   * compare/dereference directly during that window (see #get_step_data). Once the step is
   * committed, #step_store_id_refs clears #ptr to null and serializes `name` +
   * `library_filepath_abs` instead, and #step_resolve_id_refs re-resolves #ptr fresh from
   * the current #Main at the top of every later #step_decode (undo/redo) -- this is what
   * lets undo data survive object rename and correctly distinguish same-named objects from
   * different libraries, unlike a raw `id.name` string compare (which is only unique within a
   * single #Library). #ptr may be null after a resolve if the object was since deleted or could
   * not be found in the current #Main -- callers must null-check before dereferencing. */
  UndoRefID_Object object_ref = {};

  /** Name of the object's active shape key when the undo step was created. */
  std::string active_shape_key_name;

  /* TODO: Combine the three structs into a variant, since they specify data that is only valid
   * within a single mode. */
  struct {
    /* The number of vertices in the entire mesh. */
    int verts_num;
    /* The number of face corners in the entire mesh. */
    int corners_num;
  } mesh;

  struct {
    /** The number of grids in the entire mesh. A copy of #SubdivCCG::grids_num. */
    int grids_num;
    /** A copy of #SubdivCCG::grid_size. */
    int grid_size;
  } grids;

  struct {
    /**
     * The current log entry for the given BMLog step. Represents the most recent step at the time
     * that this entry is added.
     *
     * There are two usages of this pointer:
     * - If undoing or redoing a enter / exit from Dyntopo, this entry is used to rebuild the
     *   BMLog from all of the relevant entries
     * - When an undo step is no longer valid, this is used to free the data that it holds and
     *   remove it from the underlying list.
     */
    BMLogEntry *bm_entry;

    /* Geometry at the bmesh enter moment. */
    NodeGeometry geometry_enter;
  } bmesh;

  float3 pivot_pos;
  float4 pivot_rot;

  /** Origin Correct: snapshot of a rigid-body secondary object's #Object::object_to_world() at
   * session start, used for both mid-session cancel (#restore_object_transform_from_undo_step,
   * read-only) and the Ctrl+Z stack-level undo/redo swap (#restore_list_object). Unused (\a
   * has_object_transform stays false) for the active object and for any secondary object that
   * was not being origin-corrected when the session started. */
  float4x4 object_transform = float4x4::identity();
  bool has_object_transform = false;

  /* Geometry modification operations. */
  /* Original geometry is stored before the modification and is restored from when undoing. */
  NodeGeometry geometry_original;
  /* Modified geometry is stored after the modification and is restored from when redoing. */
  NodeGeometry geometry_modified;

  /**
   * Wrapped in a unique_ptr so that #StepData remains movable (a #Mutex is neither copyable nor
   * movable), which is required for storing it in a #Vector.
   */
  std::unique_ptr<Mutex> nodes_mutex = std::make_unique<Mutex>();

  /**
   * #undo::Node is stored per #pbvh::Node to reduce data storage needed for changes only impacting
   * small portions of the mesh. During undo step creation and brush evaluation we often need to
   * look up the undo state for a specific node. That lookup must be protected by a lock since
   * nodes are pushed from multiple threads. This map speeds up undo node access to reduce the
   * amount of time we wait for the lock.
   *
   * This is only accessible when building the undo step, in between #push_begin and #push_end.
   */
  Map<const bke::pbvh::Node *, std::unique_ptr<Node>> undo_nodes_by_pbvh_node;

  /** Storage of per-node undo data after creation of the undo step is finished. */
  Vector<std::unique_ptr<Node>> nodes;
  std::unique_ptr<PositionUndoStorage> position_step_storage;
  size_t undo_size;

  /**
   * Sculpt layers: explicit per-element deltas recorded into this step for the active layer.
   * Undo subtracts the delta from the live layer data, redo adds it. #sculpt_layer_uid is 0 when
   * no layer was recorded.
   *
   * Mesh (vertex) path (#sculpt_layer_grids == false): #sculpt_layer_data holds per-vertex deltas
   * at the vertices listed in #sculpt_layer_verts, keeping undo memory proportional to the
   * brushed area rather than the whole mesh. The two arrays are not tightly packed: each touched
   * PBVH node owns a contiguous slot, and only the slot's leading #sculpt_layer_seg_count[s]
   * entries (starting at #sculpt_layer_seg_start[s]) are valid, the trailing entries being unused
   * zero-delta holes. This lets stroke end skip the sequential compact pass (which was on the hot
   * path); restore iterates the segments instead. When the segment table is empty the arrays are
   * tightly packed (legacy).
   *
   * Multires/grid path (#sculpt_layer_grids == true): #sculpt_layer_verts holds the indices of
   * the grids touched by the stroke, and #sculpt_layer_data holds `grid_area` tangent-space
   * deltas per grid, in the order of the grid list (see
   * #multiresModifier_reshapeFromCCG_into_sculpt_layer). The CCG positions are not restored
   * directly for these steps: the layer delta is reverted and the composed surface re-evaluated.
   */
  int sculpt_layer_uid = 0;
  bool sculpt_layer_grids = false;
  Vector<int> sculpt_layer_verts;
  Vector<float3> sculpt_layer_data;
  Vector<int> sculpt_layer_seg_start;
  Vector<int> sculpt_layer_seg_count;

  /** Operator-level sculpt-layer undo (#Type::SculptLayer): captures metadata and optionally a
   * full data snapshot for reversible influence/visibility and data-edit operations, plus
   * tree changes (add / remove / duplicate / merge / bake, folder create / disband) as full node
   * payloads whose data ownership toggles between the undo step and the tree. Keyed by #uid so
   * restoring finds the right node even after the tree has been reordered. */
  struct SculptLayerOpUndo {
    /** #SculptLayerTreeNode::uid of the node whose metadata was captured, of either kind (0 = no
     * metadata captured, *not* the root folder — see #sculpt_layer_find). One uid counter spans
     * layers and folders, so this needs no companion "which kind" field: the restore resolves the
     * node once and swaps whatever that node has. */
    int uid = 0;
    /** Layer-only, and only meaningful when #uid names a layer; a folder has no influence. */
    float influence = 1.0f;
    int flag = 0;
    /** #eSculptLayerColorTag of the node named by #uid, swapped with the live one exactly like
     * #flag. Folders are the only nodes that ever carry a non-default value today. */
    int8_t color_tag = SCULPT_LAYER_COLOR_NONE;
    /** Swapped with the live name like #influence / #flag; for operators that leave the name alone
     * the swap is simply a no-op, so it needs no separate "was captured" marker. */
    std::string name;
    Array<float3> data;
    bool has_data = false;

    /** Layers removed by the operator (held by the step after the operator ran). */
    Vector<SculptLayerUndoPayload> removed;
    /** Layers added by the operator (in the mesh after the operator ran; extracted on undo). */
    Vector<SculptLayerUndoPayload> added;
    /** Layers whose data buffer the operator replaced wholesale, keyed by #uid. Unlike the
     * in-place #data swap above, this also carries #totelem, so it expresses a *resize* — which is
     * what repairing a stale layer is (#SCULPT_OT_layer_validate). Each payload's buffer is
     * swapped with the live layer's on undo and swapped back on redo; the swap is its own
     * inverse, like the metadata above. */
    Vector<SculptLayerUndoPayload> resized;
    /** Bake: each removed payload's contribution is also added to / subtracted from MDisps. */
    bool is_bake = false;

    /** Bake on a mesh with relative shape keys: uid of the key block the bake created (0 when the
     * bake used another carrier). #bake_key holds that block while it is detached from the mesh
     * (between undo and redo); the step owns and frees it then. */
    int bake_key_uid = 0;
    KeyBlock *bake_key = nullptr;
    /** Position #bake_key was detached from, so the redo puts it back where it was rather than at
     * the tail. It is appended at the tail by the bake, but the user is free to reorder or add
     * blocks before the undo, and #KeyBlock::relative is a positional index into #Key::block. -1
     * while the block is attached. */
    int bake_key_index = -1;
    /** #Object::shapenr from before the undo detached #bake_key, restored when the redo reattaches
     * it (the detach itself shifts it down when the active block sat after the detached one). */
    short pre_undo_shapenr = 0;
    /** Uids of the blocks whose #KeyBlock::relative pointed *at* #bake_key when the undo detached
     * it, and were remapped to the basis so they would not dangle. The redo points them back. */
    Vector<int> relative_uids;

    /** Bake on a mesh with NO shape keys yet: this step's operator created the mesh's #Key from
     * scratch. Unlike #bake_key (which detaches/reattaches one block within an already-existing
     * key), a freshly created key is fully torn down on undo and freshly rebuilt on redo —
     * nothing referenced it before this step, so there is nothing to preserve piecemeal.
     * #bake_key_uid / #bake_key are NOT used for these steps: they stay 0 / null, see the
     * `!op.created_key` guards added to #restore_list in Task 2. */
    bool created_key = false;
    /** #Object::shapenr from before the operator ran (captured rather than assumed, though it is
     * always 1 in practice since the mesh had no #Key yet). Restored verbatim on undo; recomputed
     * from the freshly rebuilt key's block index on redo. */
    short pre_bake_shapenr = 1;

    /** Node move batch (reorder, reparent, or both): empty when the step is not a move. See
     * #ReparentMove for why each entry anchors to a neighbour uid rather than a position, and why
     * it no longer says which kind of node it moves. */
    Vector<ReparentMove> moves;

    /** Folders the operator removed / added, mirroring #removed / #added for layers exactly —
     * including #groups_added holding payloads rather than bare uids: the payload is the buffer the
     * restore extracts a folder *into*, so redo can insert it back (see
     * #push_sculpt_layer_group_list_change, which fills them from uids, and the restore branch in
     * #restore_list).
     *
     * Kept apart from #removed / #added even though the payload type is now the same, because
     * #restore_list has to straddle the #moves batch with them: a folder must exist before anything
     * moves into it, and be empty before it is removed. See the ordering note there. */
    Vector<SculptLayerUndoPayload> groups_removed;
    Vector<SculptLayerUndoPayload> groups_added;

    /** Active-layer selection change: the active uid before and after (0 = no active layer, which
     * is a legal value on both sides — hence the separate flag rather than a sentinel). */
    bool has_active_change = false;
    int active_uid_from = 0;
    int active_uid_to = 0;

    /** Batch flag swap (Solo Base, folder visibility cascade): pre-change flags of the affected
     * layers, swapped with the live ones. See #push_sculpt_layer_flags_batch. */
    Vector<int> flags_batch_uids;
    Vector<int> flags_batch_flags;

    /** #SculptLayerTreeNode::uid of the node whose weight mask this step swaps, of either kind
     * (0 = no mask captured). Separate from #uid rather than folded into it because the two are
     * captured by different operators: a mask operator changes no metadata, and reusing #uid would
     * make the restore swap this step's empty #name onto the node. */
    int mask_uid = 0;
    /** The mask the node carried before the operator ran, owned by the step and swapped with the
     * live one on restore. Null is a legal captured state ("the node had no mask"), which is why
     * #mask_uid and not this pointer says whether anything was captured. */
    SculptLayerMask *mask_swap = nullptr;

    /** Non-zero when this step opens or closes a mask editing session; holds the node's uid. */
    int mask_session_uid = 0;
    /** Whether the step *opened* the session named by #mask_session_uid (as opposed to closing it).
     * See #mask_session_boundary for how the two combine with the restore direction. */
    bool mask_session_entering = false;

    SculptLayerOpUndo() = default;
    /* Move-only and owning, mirroring #SculptLayerUndoPayload: #mask_swap is a raw owning pointer,
     * and an implicit copy would hand the same mask to two steps to free. Nothing moves or copies a
     * #StepData today (it holds a #Mutex and a map of unique pointers), so the copy operations are
     * deleted rather than replaced by move ones that no caller needs. */
    SculptLayerOpUndo(const SculptLayerOpUndo &) = delete;
    SculptLayerOpUndo &operator=(const SculptLayerOpUndo &) = delete;
    ~SculptLayerOpUndo()
    {
      bke::sculpt_layers::mask_free(mask_swap);
    }
  } sculpt_layer_op;

  /** Whether processing code needs to handle the current data as an undo step. */
  bool needs_undo() const
  {
    return applied_;
  }

  void tag_needs_undo()
  {
    applied_ = true;
  }

  void tag_needs_redo()
  {
    applied_ = false;
  }
};

namespace compression {

/**
 * Compress a span, using a prefiltering step that can improve compression speed and ratios for
 * certain float data types.
 */
template<typename T>
void filter_compress(const Span<T> src,
                     Vector<std::byte> &filter_buffer,
                     Vector<std::byte> &compress_buffer)
{
  PRF_scope(ProfileCategory::Editor);
  filter_buffer.resize(src.size_in_bytes());
  filter_transpose_delta(reinterpret_cast<const uint8_t *>(src.data()),
                         reinterpret_cast<uint8_t *>(filter_buffer.data()),
                         src.size(),
                         sizeof(T));

  /* Level 3 gives a good balance of compression performance and ratio, and is also used elsewhere
   * across Blender for calls to #ZSTD_compress. */
  constexpr int zstd_level = 3;
  compress_buffer.resize(ZSTD_compressBound(src.size_in_bytes()));
  const size_t dst_size = ZSTD_compress(compress_buffer.data(),
                                        compress_buffer.size(),
                                        filter_buffer.data(),
                                        filter_buffer.size(),
                                        zstd_level);
  if (ZSTD_isError(dst_size)) {
    compress_buffer.clear();
    return;
  }

  compress_buffer.resize(dst_size);
}

template<typename T>
void filter_decompress(const Span<std::byte> src, Vector<std::byte> &buffer, Vector<T> &dst)
{
  PRF_scope(ProfileCategory::Editor);
  const unsigned long long dst_size_in_bytes = ZSTD_getFrameContentSize(src.data(), src.size());
  if (ELEM(dst_size_in_bytes, ZSTD_CONTENTSIZE_ERROR, ZSTD_CONTENTSIZE_UNKNOWN)) {
    dst.clear();
    return;
  }

  buffer.resize(dst_size_in_bytes);
  const size_t result = ZSTD_decompress(buffer.data(), buffer.size(), src.data(), src.size());
  if (ZSTD_isError(result)) {
    dst.clear();
    return;
  }

  dst.resize(buffer.size() / sizeof(T));
  unfilter_transpose_delta(reinterpret_cast<const uint8_t *>(buffer.data()),
                           reinterpret_cast<uint8_t *>(dst.data()),
                           dst.size(),
                           sizeof(T));
}

template void filter_compress<float3>(Span<float3>, Vector<std::byte> &, Vector<std::byte> &);
template void filter_compress<int>(Span<int>, Vector<std::byte> &, Vector<std::byte> &);

template void filter_decompress<float3>(Span<std::byte>, Vector<std::byte> &, Vector<float3> &);
template void filter_decompress<int>(Span<std::byte>, Vector<std::byte> &, Vector<int> &);

}  // namespace compression

struct PositionUndoStorage : NonMovable {
  Vector<std::unique_ptr<Node>> nodes_to_compress;
  bool multires_undo;

  Array<Array<std::byte>> compressed_indices;

  /* As undo and redo happen, the data in these arrays is swapped (an undo step becomes a redo
   * step, and vice versa). */
  Array<Array<std::byte>> compressed_positions;

  Array<int> unique_verts_nums;

  TaskPool *compression_task_pool;
  std::atomic<bool> compression_ready = false;
  std::atomic<bool> compression_started = false;
  StepData *owner_step_data = nullptr;

  explicit PositionUndoStorage(StepData &step_data)
      : nodes_to_compress(std::move(step_data.nodes)), owner_step_data(&step_data)
  {
    this->multires_undo = step_data.grids.grids_num != 0;
    if (!multires_undo) {
      this->unique_verts_nums.reinitialize(this->nodes_to_compress.size());
      for (const int i : this->nodes_to_compress.index_range()) {
        this->unique_verts_nums[i] = this->nodes_to_compress[i]->unique_verts_num;
      }
    }

    this->compression_task_pool = BLI_task_pool_create_background(this, TASK_PRIORITY_LOW);
    this->compression_started = true;

    BLI_task_pool_push(this->compression_task_pool, compress_fn, this, false, nullptr);
  }

  ~PositionUndoStorage()
  {
    if (compression_started.load() && compression_task_pool) {
      BLI_task_pool_work_and_wait(compression_task_pool);
      BLI_task_pool_free(compression_task_pool);
    }
  }

  void ensure_compression_complete()
  {
    if (!compression_ready.load(std::memory_order_acquire)) {
      BLI_task_pool_work_and_wait(compression_task_pool);
    }
  }

  static void compress_fn(TaskPool * /*pool*/, void *task_data)
  {
#ifdef DEBUG_TIME
    SCOPED_TIMER_AVERAGED(__func__);
#endif
    auto *data = static_cast<PositionUndoStorage *>(task_data);
    MutableSpan<std::unique_ptr<Node>> nodes = data->nodes_to_compress;
    const int nodes_num = nodes.size();

    Array<Array<std::byte>> compressed_indices(nodes.size(), NoInitialization());
    Array<Array<std::byte>> compressed_data(nodes.size(), NoInitialization());
    struct CompressLocalData {
      Vector<std::byte> filtered;
      Vector<std::byte> compressed;
    };
    threading::isolate_task([&]() {
      threading::EnumerableThreadSpecific<CompressLocalData> all_tls;
      threading::parallel_for(IndexRange(nodes_num), 1, [&](const IndexRange range) {
        CompressLocalData &local_data = all_tls.local();
        for (const int i : range) {
          const Span<int> indices = data->multires_undo ? nodes[i]->grids : nodes[i]->vert_indices;
          const Span<float3> positions = !nodes[i]->orig_position.is_empty() ?
                                             nodes[i]->orig_position :
                                             nodes[i]->position;
          compression::filter_compress(indices, local_data.filtered, local_data.compressed);
          new (&compressed_indices[i]) Array<std::byte>(local_data.compressed.as_span());
          compression::filter_compress(positions, local_data.filtered, local_data.compressed);
          new (&compressed_data[i]) Array<std::byte>(local_data.compressed.as_span());
          nodes[i].reset();
        }
      });
    });
    data->nodes_to_compress.clear_and_shrink();

    size_t memory_size = 0;
    for (const int i : IndexRange(nodes_num)) {
      memory_size += compressed_indices[i].as_span().size_in_bytes();
      memory_size += compressed_data[i].as_span().size_in_bytes();
    }

    data->compressed_indices = std::move(compressed_indices);
    data->compressed_positions = std::move(compressed_data);
    data->owner_step_data->undo_size += memory_size;

    data->compression_ready.store(true, std::memory_order_release);
  }
};

struct SculptUndoStep {
  UndoStep step;
  /**
   * List of StepData for each object involved in the undo step.
   * This supports multi-object sculpt mode.
   *
   * Stored behind a #unique_ptr so that a #StepData pointer handed out by #get_step_data stays
   * valid even when the vector grows: undo nodes are pushed concurrently from worker threads
   * (see #ensure_node), and a reallocating #Vector<StepData> would move the elements out from
   * under those threads.
   */
  Vector<std::unique_ptr<StepData>> objects_data;

  /* Active color attribute at the start of this undo step. */
  SculptAttrRef active_color_start;

  /* Active color attribute at the end of this undo step. */
  SculptAttrRef active_color_end;
};

size_t step_memory_size_get(UndoStep *step)
{
  if (step->type != BKE_UNDOSYS_TYPE_SCULPT) {
    return 0;
  }

  SculptUndoStep *sculpt_step = reinterpret_cast<SculptUndoStep *>(step);
  size_t total_size = 0;
  for (std::unique_ptr<StepData> &sd : sculpt_step->objects_data) {
    if (sd->position_step_storage) {
      sd->position_step_storage->ensure_compression_complete();
    }
    total_size += sd->undo_size;
  }

  return total_size;
}

/**
 * Return the sculpt undo step currently being built (#UndoStack.step_init).
 * Must not fall back to the last applied sculpt step: doing so would write stroke undo data into
 * an already committed step, or finalize/destroy the wrong step in #push_end_all_ex.
 *
 * This replaces the bundle's `get_active_step` / object-less `get_step_data()` pair outright: on
 * this fork a step owns one #StepData per participating object (#SculptUndoStep.objects_data), so
 * there is no single step-wide #StepData for an object-less accessor to return. Every layer-undo
 * caller must go through #get_step_data(const Object &) below.
 */
static SculptUndoStep *get_init_sculpt_step()
{
  UndoStack *ustack = ED_undo_stack_get();
  if (ustack->step_init && ustack->step_init->type == BKE_UNDOSYS_TYPE_SCULPT) {
    return reinterpret_cast<SculptUndoStep *>(ustack->step_init);
  }
  return nullptr;
}

static bool step_data_has_undo_content(const StepData &step_data)
{
  if (!step_data.undo_nodes_by_pbvh_node.is_empty()) {
    return true;
  }
  if (!step_data.nodes.is_empty()) {
    return true;
  }
  if (step_data.position_step_storage) {
    return true;
  }
  return step_data.type != Type::None;
}

static bool sculpt_step_has_undo_content(const SculptUndoStep &us)
{
  for (const std::unique_ptr<StepData> &step_data : us.objects_data) {
    if (step_data_has_undo_content(*step_data)) {
      return true;
    }
  }
  return false;
}

static void discard_init_sculpt_step()
{
  UndoStack *ustack = ED_undo_stack_get();
  UndoStep *us = ustack->step_init;
  if (!us || us->type != BKE_UNDOSYS_TYPE_SCULPT) {
    return;
  }
  us->type->step_free(us);
  MEM_delete(us);
  ustack->step_init = nullptr;
}

static bool sculpt_step_should_push(const SculptUndoStep &us)
{
  if (sculpt_step_has_undo_content(us)) {
    return true;
  }
  /* Enter sculpt mode records objects without topology counts; those steps must still be pushed
   * so undo can leave sculpt mode. Stroke steps always store topology in #save_step_topology_data.
   */
  for (const std::unique_ptr<StepData> &sd : us.objects_data) {
    if (sd->type == Type::None && sd->mesh.verts_num == 0 && sd->grids.grids_num == 0) {
      return true;
    }
  }
  return false;
}

/**
 * Compares #StepData.object_ref.ptr directly against `&ob`. Safe ONLY while the step is still
 * being built (`ustack->step_init`): #object_ref.ptr is a live pointer from creation
 * (#get_step_data below) until the step is committed, at which point the generic undo system
 * clears it to null (see #StepData.object_ref's doc-comment) -- #find_step_data is never called
 * after that point, since #get_step_data only ever operates on the in-progress init step.
 *
 * Caller must hold at least a shared lock on #objects_data_mutex.
 */
static StepData *find_step_data(SculptUndoStep &us, const Object &ob)
{
  for (std::unique_ptr<StepData> &sd : us.objects_data) {
    if (sd->object_ref.ptr == &ob) {
      return sd.get();
    }
  }
  return nullptr;
}

static StepData *get_step_data(const Object &ob)
{
  SculptUndoStep *us = get_init_sculpt_step();
  if (!us) {
    return nullptr;
  }

  /* Undo nodes are pushed and looked up concurrently from worker threads (see #push_node /
   * #ensure_node / #get_node), so access to #objects_data must be serialized. The returned
   * #StepData itself is heap-allocated via #unique_ptr, so it stays valid for the caller even if
   * another thread appends afterwards.
   *
   * By the time a multi-object stroke's per-node worker threads start, #StepData already exists
   * for every participating object -- entries are created on the main thread by
   * #push_begin / #push_begin_add_object before the parallel per-node work begins (see
   * #save_step_topology_data). So the hot path from every worker thread is a pure lookup; only
   * the (effectively single-threaded, first-touch) creation of a new #StepData mutates
   * #objects_data. A #std::shared_mutex lets concurrent lookups run in parallel with each other,
   * serializing only the rare append -- unlike a plain #Mutex, which serialized every lookup
   * against every other lookup regardless of whether either one ever mutated the vector. */
  static std::shared_mutex objects_data_mutex;

  {
    std::shared_lock read_lock(objects_data_mutex);
    if (StepData *found = find_step_data(*us, ob)) {
      return found;
    }
  }

  /* Not found under a shared lock: escalate to exclusive to append. #std::shared_mutex has no
   * atomic shared-to-exclusive upgrade, so re-check after acquiring the exclusive lock in case
   * another thread inserted this object's #StepData in between. */
  std::unique_lock write_lock(objects_data_mutex);
  if (StepData *found = find_step_data(*us, ob)) {
    return found;
  }

  us->objects_data.append(std::make_unique<StepData>());
  StepData &sd = *us->objects_data.last();
  sd.object_ref.ptr = &const_cast<Object &>(ob);
  return &sd;
}

static bool use_multires_undo(const StepData &step_data, const SculptSession &ss)
{
  return step_data.grids.grids_num != 0 && ss.subdiv_ccg != nullptr;
}

static bool topology_matches(const StepData &step_data,
                             const Object &object,
                             const bke::pbvh::Tree &pbvh)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bool multires_undo_step = use_multires_undo(step_data, ss);
  /* #BKE_sculpt_update_object_for_edit (called just before this) may have refreshed
   * #SculptSession::subdiv_ccg (e.g. a multires modifier's grids becoming available or
   * unavailable), which can change what #use_multires_undo() returns compared to when
   * #pbvh's type was last determined. Re-check here, right before the caller dispatches to
   * #pbvh.nodes<GridsNode>() or #pbvh.nodes<MeshNode>(), to avoid requesting the wrong node
   * type and crashing. See #131478. */
  if (multires_undo_step != (pbvh.type() == bke::pbvh::Type::Grids)) {
    return false;
  }
  if (multires_undo_step) {
    const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
    return subdiv_ccg.grids_num == step_data.grids.grids_num &&
           subdiv_ccg.grid_size == step_data.grids.grid_size;
  }
  const Mesh &mesh = *id_cast<Mesh *>(object.data);
  return mesh.verts_num == step_data.mesh.verts_num;
}

static void save_mesh_topology_data(const Object &ob, StepData &step_data)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  step_data.mesh.verts_num = mesh.verts_num;
  step_data.mesh.corners_num = mesh.corners_num;
}

static void save_grids_topology_data(const SculptSession &ss, StepData &step_data)
{
  if (ss.subdiv_ccg) {
    step_data.grids.grids_num = ss.subdiv_ccg->grids_num;
    step_data.grids.grid_size = ss.subdiv_ccg->grid_size;
  }
}

static void ensure_step_topology_data(Object &object, StepData &step_data)
{
  if (step_data.mesh.verts_num != 0 || step_data.grids.grids_num != 0) {
    return;
  }

  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh) {
    switch (pbvh->type()) {
      case bke::pbvh::Type::Mesh:
        save_mesh_topology_data(object, step_data);
        break;
      case bke::pbvh::Type::Grids:
        save_grids_topology_data(ss, step_data);
        break;
      case bke::pbvh::Type::BMesh:
        break;
    }
  }
  else {
    save_mesh_topology_data(object, step_data);
    save_grids_topology_data(ss, step_data);
  }
}

static bool indices_contain_true(const Span<bool> data, const Span<int> indices)
{
  return std::any_of(indices.begin(), indices.end(), [&](const int i) { return data[i]; });
}

static bool restore_active_shape_key(bContext &C,
                                     Depsgraph &depsgraph,
                                     const StepData &step_data,
                                     Object &object)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  if (ss.shapekey_active && ss.shapekey_active->name != step_data.active_shape_key_name) {
    /* Shape key has been changed before calling undo operator. */

    Key *key = BKE_key_from_object(&object);
    const KeyBlock *kb = key ?
                             BKE_keyblock_find_name(key, step_data.active_shape_key_name.c_str()) :
                             nullptr;

    if (kb) {
      object.shapenr = BLI_findindex(&key->block, kb) + 1;

      BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
      WM_event_add_notifier(&C, NC_OBJECT | ND_DATA, &object);
    }
    else {
      /* Key has been removed -- skip this undo node. */
      return false;
    }
  }
  return true;
}

template<typename T>
static void swap_indexed_data(MutableSpan<T> full, const Span<int> indices, MutableSpan<T> indexed)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(full.size() == indices.size());
  for (const int i : indices.index_range()) {
    std::swap(full[i], indexed[indices[i]]);
  }
}

static void restore_position_mesh(Object &object,
                                  PositionUndoStorage &undo_data,
                                  const MutableSpan<bool> modified_verts,
                                  const bool skip_positions_swap)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  std::optional<ShapeKeyData> shape_key_data = ShapeKeyData::from_object(object);

  undo_data.ensure_compression_complete();

  const int nodes_num = undo_data.unique_verts_nums.size();

  struct LocalData {
    Vector<std::byte> compress_buffer;
    Vector<std::byte> filter_buffer;
    Vector<int> indices;
    Vector<float3> positions;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  threading::parallel_for(IndexRange(nodes_num), 1, [&](const IndexRange range) {
    LocalData &tls = all_tls.local();
    for (const int i : range) {
      compression::filter_decompress<int>(
          undo_data.compressed_indices[i], tls.compress_buffer, tls.indices);
      const int unique_verts_num = undo_data.unique_verts_nums[i];
      const Span<int> verts = tls.indices.as_span().take_front(unique_verts_num);

      compression::filter_decompress<float3>(
          undo_data.compressed_positions[i], tls.compress_buffer, tls.positions);
      MutableSpan undo_positions = tls.positions.as_mutable_span();

      if (!ss.deform_modifiers_active && !shape_key_data) {
        /* When original positions aren't written separately in the undo step, there are no
         * deform modifiers. Therefore the original and evaluated deform positions will be the
         * same, and modifying the positions from the original mesh is enough. */
        if (!skip_positions_swap) {
          swap_indexed_data(undo_positions.take_front(unique_verts_num), verts, positions);
        }
        else {
          undo_positions.take_front(unique_verts_num);
        }
      }
      else {
        /* When original positions are stored in the undo step, undo/redo will cause a reevaluation
         * of the object. The evaluation will recompute the evaluated positions, so dealing with
         * them here is unnecessary. */
        if (shape_key_data) {
          MutableSpan<float3> active_data = shape_key_data->active_key_data;

          if (!shape_key_data->dependent_keys.is_empty()) {
            Array<float3, 1024> translations(verts.size());
            translations_from_new_positions(
                undo_positions.take_front(unique_verts_num), verts, active_data, translations);
            for (MutableSpan<float3> data : shape_key_data->dependent_keys) {
              apply_translations(translations, verts, data);
            }
          }

          if (shape_key_data->basis_key_active) {
            /* The basis key positions and the mesh positions are always kept in sync. */
            scatter_data_mesh(undo_positions.as_span(), verts, positions);
          }
          if (!skip_positions_swap) {
            swap_indexed_data(undo_positions.take_front(unique_verts_num), verts, active_data);
          }
          else {
            undo_positions.take_front(unique_verts_num);
          }
        }
        else {
          /* There is a deform modifier, but no shape keys. */
          if (!skip_positions_swap) {
            swap_indexed_data(undo_positions.take_front(unique_verts_num), verts, positions);
          }
          else {
            undo_positions.take_front(unique_verts_num);
          }
        }
      }

      modified_verts.fill_indices(verts, true);

      compression::filter_compress<float3>(undo_positions, tls.filter_buffer, tls.compress_buffer);
      undo_data.compressed_positions[i] = tls.compress_buffer.as_span();
    }
  });
}

static void restore_position_grids(const MutableSpan<float3> positions,
                                   const CCGKey &key,
                                   PositionUndoStorage &undo_data,
                                   const MutableSpan<bool> modified_grids)
{
  PRF_scope(ProfileCategory::Editor);
  const int nodes_num = undo_data.compressed_indices.size();

  struct LocalData {
    Vector<std::byte> compress_buffer;
    Vector<std::byte> filter_buffer;
    Vector<int> indices;
    Vector<float3> positions;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  threading::parallel_for(IndexRange(nodes_num), 1, [&](const IndexRange range) {
    LocalData &tls = all_tls.local();
    for (const int i : range) {
      compression::filter_decompress<int>(
          undo_data.compressed_indices[i], tls.compress_buffer, tls.indices);
      const Span<int> grids = tls.indices.as_span();

      compression::filter_decompress<float3>(
          undo_data.compressed_positions[i], tls.compress_buffer, tls.positions);
      MutableSpan node_positions = tls.positions.as_mutable_span();

      for (const int i : grids.index_range()) {
        MutableSpan data = positions.slice(bke::ccg::grid_range(key, grids[i]));
        MutableSpan undo_data = node_positions.slice(bke::ccg::grid_range(key, i));
        for (const int offset : data.index_range()) {
          std::swap(data[offset], undo_data[offset]);
        }
      }

      modified_grids.fill_indices(grids, true);

      compression::filter_compress<float3>(node_positions, tls.filter_buffer, tls.compress_buffer);
      undo_data.compressed_positions[i] = tls.compress_buffer.as_span();
    }
  });
}

static void restore_vert_visibility_mesh(Object &object,
                                         Node &unode,
                                         const MutableSpan<bool> modified_verts)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<bool> hide_vert = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_vert", bke::AttrDomain::Point);
  for (const int i : unode.vert_indices.index_range().take_front(unode.unique_verts_num)) {
    const int vert = unode.vert_indices[i];
    if (unode.vert_hidden[i].test() != hide_vert.span[vert]) {
      unode.vert_hidden[i].set(!unode.vert_hidden[i].test());
      hide_vert.span[vert] = !hide_vert.span[vert];
      modified_verts[vert] = true;
    }
  }
  hide_vert.finish();
}

static void restore_vert_visibility_grids(SubdivCCG &subdiv_ccg,
                                          Node &unode,
                                          const MutableSpan<bool> modified_grids)
{
  PRF_scope(ProfileCategory::Editor);
  if (unode.grid_hidden.is_empty()) {
    BKE_subdiv_ccg_grid_hidden_free(subdiv_ccg);
    return;
  }

  BitGroupVector<> &grid_hidden = BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg);
  const Span<int> grids = unode.grids;
  for (const int i : grids.index_range()) {
    /* Swap the two bit spans. */
    MutableBoundedBitSpan a = unode.grid_hidden[i];
    MutableBoundedBitSpan b = grid_hidden[grids[i]];
    for (const int j : a.index_range()) {
      const bool value_a = a[j];
      const bool value_b = b[j];
      a[j].set(value_b);
      b[j].set(value_a);
    }
  }

  modified_grids.fill_indices(grids, true);
}

static void restore_hidden_face(Object &object,
                                Node &unode,
                                const MutableSpan<bool> modified_faces)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter hide_poly = attributes.lookup_or_add_for_write_span<bool>(
      ".hide_poly", bke::AttrDomain::Face);

  const Span<int> face_indices = unode.face_indices;

  for (const int i : face_indices.index_range()) {
    const int face = face_indices[i];
    if (unode.face_hidden[i].test() != hide_poly.span[face]) {
      unode.face_hidden[i].set(!unode.face_hidden[i].test());
      hide_poly.span[face] = !hide_poly.span[face];
      modified_faces[face] = true;
    }
  }
  hide_poly.finish();
}

static void restore_color(Object &object,
                          StepData &step_data,
                          const MutableSpan<bool> modified_verts)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  bke::GSpanAttributeWriter color_attribute = color::active_color_attribute_for_write(mesh);

  for (std::unique_ptr<Node> &unode : step_data.nodes) {
    if (color_attribute.domain == bke::AttrDomain::Point && !unode->col.is_empty()) {
      color::swap_gathered_colors(
          unode->vert_indices.as_span().take_front(unode->unique_verts_num),
          color_attribute.span,
          unode->col);
    }
    else if (color_attribute.domain == bke::AttrDomain::Corner && !unode->loop_col.is_empty()) {
      color::swap_gathered_colors(unode->corner_indices, color_attribute.span, unode->loop_col);
    }

    modified_verts.fill_indices(unode->vert_indices.as_span(), true);
  }

  color_attribute.finish();
}

static void restore_mask_mesh(Object &object, Node &unode, const MutableSpan<bool> modified_verts)
{
  PRF_scope(ProfileCategory::Editor);
  Mesh *mesh = BKE_object_get_original_mesh(&object);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", bke::AttrDomain::Point);

  const Span<int> index = unode.vert_indices.as_span().take_front(unode.unique_verts_num);

  for (const int i : index.index_range()) {
    const int vert = index[i];
    if (mask.span[vert] != unode.mask[i]) {
      std::swap(mask.span[vert], unode.mask[i]);
      modified_verts[vert] = true;
    }
  }

  mask.finish();
}

static void restore_mask_grids(Object &object, Node &unode, const MutableSpan<bool> modified_grids)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG *subdiv_ccg = ss.subdiv_ccg;
  MutableSpan<float> masks = subdiv_ccg->masks;

  const CCGKey key = BKE_subdiv_ccg_key_top_level(*subdiv_ccg);

  const Span<int> grids = unode.grids;
  MutableSpan<float> undo_mask = unode.mask;

  for (const int i : grids.index_range()) {
    MutableSpan data = masks.slice(bke::ccg::grid_range(key, grids[i]));
    MutableSpan undo_data = undo_mask.slice(bke::ccg::grid_range(key, i));
    for (const int offset : data.index_range()) {
      std::swap(data[offset], undo_data[offset]);
    }
  }

  modified_grids.fill_indices(unode.grids.as_span(), true);
}

static bool restore_face_sets(Object &object,
                              Node &unode,
                              const MutableSpan<bool> modified_face_set_faces)
{
  PRF_scope(ProfileCategory::Editor);
  const Span<int> face_indices = unode.face_indices;

  bke::SpanAttributeWriter<int> face_sets = face_set::ensure_face_sets_mesh(
      *id_cast<Mesh *>(object.data));
  bool modified = false;
  for (const int i : face_indices.index_range()) {
    const int face = face_indices[i];
    if (unode.face_sets[i] == face_sets.span[face]) {
      continue;
    }
    std::swap(unode.face_sets[i], face_sets.span[face]);
    modified_face_set_faces[face] = true;
    modified = true;
  }
  face_sets.finish();
  return modified;
}

static void bmesh_restore_generic(StepData &step_data, Object &object)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;
  if (step_data.needs_undo()) {
    BM_log_undo(ss.bm, ss.bm_log);
    step_data.tag_needs_redo();
  }
  else {
    BM_log_redo(ss.bm, ss.bm_log);
    step_data.tag_needs_undo();
  }

  if (step_data.type == Type::Mask) {
    bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
    IndexMaskMemory memory;
    const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);
    pbvh.tag_masks_changed(node_mask);
    bke::pbvh::update_mask_bmesh(*ss.bm, node_mask, pbvh);
  }
  else {
    BKE_sculptsession_free_pbvh(object);
    DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
    BM_mesh_normals_update(ss.bm);
  }
}

/* Create empty sculpt BMesh and enable logging. */
static void bmesh_enable(Object &object, const StepData &step_data)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  Mesh *mesh = id_cast<Mesh *>(object.data);

  BKE_sculptsession_free_pbvh(object);
  DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);

  /* Create empty BMesh and enable logging. */
  BMeshCreateParams bmesh_create_params{};
  bmesh_create_params.use_toolflags = false;

  ss.bm = BM_mesh_create(&bm_mesh_allocsize_default, &bmesh_create_params);
  BM_data_layer_add_named(ss.bm, &ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");

  mesh->flag |= ME_SCULPT_DYNAMIC_TOPOLOGY;

  /* Restore the BMLog using saved entries. */
  ss.bm_log = BM_log_from_existing_entries_create(ss.bm, step_data.bmesh.bm_entry);
}

static void bmesh_handle_dyntopo_begin(bContext *C, StepData &step_data, Object &object)
{
  if (step_data.needs_undo()) {
    dyntopo::disable(C, &step_data);
    step_data.tag_needs_redo();
  }
  else /* needs_redo */ {
    SculptSession &ss = *object.runtime->sculpt_session;
    bmesh_enable(object, step_data);

    /* Restore the mesh from the first log entry. */
    BM_log_redo(ss.bm, ss.bm_log);

    step_data.tag_needs_undo();
  }
}

static void bmesh_handle_dyntopo_end(bContext *C, StepData &step_data, Object &object)
{
  if (step_data.needs_undo()) {
    SculptSession &ss = *object.runtime->sculpt_session;
    bmesh_enable(object, step_data);

    /* Restore the mesh from the last log entry. */
    BM_log_undo(ss.bm, ss.bm_log);

    step_data.tag_needs_redo();
  }
  else /* needs_redo */ {
    /* Disable dynamic topology sculpting. */
    dyntopo::disable(C, nullptr);
    step_data.tag_needs_undo();
  }
}

static void store_geometry_data(NodeGeometry *geometry, const Object &object)
{
  PRF_scope(ProfileCategory::Editor);
  const Mesh *mesh = id_cast<const Mesh *>(object.data);

  BLI_assert(!geometry->is_initialized);
  geometry->is_initialized = true;

  geometry->attribute_storage = mesh->attribute_storage.wrap();
  CustomData_init_from(
      &mesh->vert_data, &geometry->vert_data, CD_MASK_MESH.vmask, mesh->verts_num);
  CustomData_init_from(
      &mesh->edge_data, &geometry->edge_data, CD_MASK_MESH.emask, mesh->edges_num);
  CustomData_init_from(
      &mesh->corner_data, &geometry->corner_data, CD_MASK_MESH.lmask, mesh->corners_num);
  CustomData_init_from(
      &mesh->face_data, &geometry->face_data, CD_MASK_MESH.pmask, mesh->faces_num);
  implicit_sharing::copy_shared_pointer(mesh->face_offset_indices,
                                        mesh->runtime->face_offsets_sharing_info,
                                        &geometry->face_offset_indices,
                                        &geometry->face_offsets_sharing_info);

  geometry->verts_num = mesh->verts_num;
  geometry->edges_num = mesh->edges_num;
  geometry->corners_num = mesh->corners_num;
  geometry->faces_num = mesh->faces_num;
}

static void restore_geometry_data(const NodeGeometry *geometry, Mesh *mesh)
{
  PRF_scope(ProfileCategory::Editor);
  BLI_assert(geometry->is_initialized);

  BKE_mesh_clear_geometry(mesh);

  mesh->verts_num = geometry->verts_num;
  mesh->edges_num = geometry->edges_num;
  mesh->corners_num = geometry->corners_num;
  mesh->faces_num = geometry->faces_num;
  mesh->totface_legacy = 0;

  mesh->attribute_storage.wrap() = geometry->attribute_storage.wrap();
  CustomData_init_from(
      &geometry->vert_data, &mesh->vert_data, CD_MASK_MESH.vmask, geometry->verts_num);
  CustomData_init_from(
      &geometry->edge_data, &mesh->edge_data, CD_MASK_MESH.emask, geometry->edges_num);
  CustomData_init_from(
      &geometry->corner_data, &mesh->corner_data, CD_MASK_MESH.lmask, geometry->corners_num);
  CustomData_init_from(
      &geometry->face_data, &mesh->face_data, CD_MASK_MESH.pmask, geometry->faces_num);
  implicit_sharing::copy_shared_pointer(geometry->face_offset_indices,
                                        geometry->face_offsets_sharing_info,
                                        &mesh->face_offset_indices,
                                        &mesh->runtime->face_offsets_sharing_info);
}

static void geometry_free_data(NodeGeometry *geometry)
{
  PRF_scope(ProfileCategory::Editor);
  CustomData_free(&geometry->vert_data);
  CustomData_free(&geometry->edge_data);
  CustomData_free(&geometry->corner_data);
  CustomData_free(&geometry->face_data);
  implicit_sharing::free_shared_data(&geometry->face_offset_indices,
                                     &geometry->face_offsets_sharing_info);
}

static void restore_geometry(StepData &step_data, Object &object)
{
  PRF_scope(ProfileCategory::Editor);
  BKE_sculptsession_free_pbvh(object);
  DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);

  Mesh *mesh = id_cast<Mesh *>(object.data);

  if (step_data.needs_undo()) {
    restore_geometry_data(&step_data.geometry_original, mesh);
    step_data.tag_needs_redo();
  }
  else {
    restore_geometry_data(&step_data.geometry_modified, mesh);
    step_data.tag_needs_undo();
  }
}

/* Handle all dynamic-topology updates
 *
 * Returns true if this was a dynamic-topology undo step, otherwise
 * returns false to indicate the non-dyntopo code should run. */
static int bmesh_restore(bContext *C, Depsgraph &depsgraph, StepData &step_data, Object &object)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  switch (step_data.type) {
    case Type::DyntopoBegin:
      BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
      bmesh_handle_dyntopo_begin(C, step_data, object);
      return true;

    case Type::DyntopoEnd:
      BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
      bmesh_handle_dyntopo_end(C, step_data, object);
      return true;
    default:
      if (ss.bm_log) {
        BKE_sculpt_update_object_for_edit(&depsgraph, &object, false);
        bmesh_restore_generic(step_data, object);
        return true;
      }
      break;
  }

  return false;
}

void restore_from_bmesh_enter_geometry(const StepData &step_data, Mesh &mesh)
{
  restore_geometry_data(&step_data.bmesh.geometry_enter, &mesh);
}

bool has_bmesh_log_entry(const Object &ob)
{
  StepData *step_data = get_step_data(ob);
  return step_data && step_data->bmesh.bm_entry;
}

/* Geometry updates (such as Apply Base, for example) will re-evaluate the object and refine its
 * Subdiv descriptor. Upon undo it is required that mesh, grids, and subdiv all stay consistent
 * with each other. This means that when geometry coordinate changes the undo should refine the
 * subdiv to the new coarse mesh coordinates. Tricky part is: this needs to happen without using
 * dependency graph tag: tagging object for geometry update will either loose sculpted data from
 * the sculpt grids, or will wrongly "commit" them to the CD_MDISPS.
 *
 * So what we do instead is do minimum object evaluation to get base mesh coordinates for the
 * multires modifier input. While this is expensive, it is less expensive than dependency graph
 * evaluation and is only happening when geometry coordinates changes on undo.
 *
 * Note that the dependency graph is ensured to be evaluated prior to the undo step is decoded,
 * so if the object's modifier stack references other object it is all fine. */
static void refine_subdiv(Depsgraph *depsgraph,
                          const SculptSession &ss,
                          Object &object,
                          bke::subdiv::Subdiv *subdiv)
{
  Array<float3> deformed_verts = BKE_multires_create_deformed_base_mesh_vert_coords(
      depsgraph, &object, ss.multires_modifier);

  bke::subdiv::eval_refine_from_mesh(subdiv, id_cast<const Mesh *>(object.data), deformed_verts);
}

/**
 * The layer with \a uid, or null when nothing on \a mesh holds it or the node holding it is a
 * folder.
 *
 * Kind-checked and 0-guarded rather than a bare #bke::sculpt_layers::node_find_by_uid, because one
 * uid counter now spans both kinds: reinterpreting whatever a uid resolves to would read a
 * #SculptLayerGroup's child list as #SculptLayer::influence and its #SculptLayer::data pointer past
 * the end of the smaller allocation. Uid 0 needs a guard of its own — #node_find_by_uid resolves it
 * to the *root folder*, whereas every uid field that reaches this function uses 0 to mean "nothing
 * recorded".
 */
static SculptLayer *sculpt_layer_find(Mesh &mesh, const int uid)
{
  if (uid == 0) {
    return nullptr;
  }
  return bke::sculpt_layers::node_as_layer(bke::sculpt_layers::node_find_by_uid(mesh, uid));
}

/**
 * Whether the recorded per-element layer deltas of a mesh (vertex domain) Position step can be
 * applied: the recorded layer must still exist with a data buffer, and the stored arrays must be
 * consistent. Used to decide whether the position restore may be delegated to
 * #restore_active_sculpt_layer (`skip_positions_swap`): if this returns false the plain position
 * swap must run instead, otherwise the step would silently restore nothing (e.g. when the layer
 * was removed out-of-band, bypassing the layer operators' undo payloads).
 */
static bool can_restore_active_sculpt_layer_mesh(const StepData &step_data, Object &object)
{
  if (step_data.sculpt_layer_uid == 0 || step_data.sculpt_layer_grids ||
      step_data.sculpt_layer_data.is_empty())
  {
    return false;
  }
  if (step_data.sculpt_layer_verts.size() != step_data.sculpt_layer_data.size()) {
    return false;
  }
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  const SculptLayer *layer = sculpt_layer_find(mesh, step_data.sculpt_layer_uid);
  return layer != nullptr && layer->data != nullptr;
}

/**
 * Sculpt layers: revert or re-apply the explicit per-element deltas recorded for the active
 * layer. The delta is constant, so undo subtracts and redo adds it (no swap needed).
 *
 * \a is_undo is threaded down from #restore_list rather than read off #StepData::needs_undo:
 * that flag is flipped only by the dyntopo and geometry restore helpers, and a #Type::Position
 * step recorded into a layer reaches none of them, so it would read a constant `true` and repeat
 * the undo subtraction on every redo (see the identical reasoning at #restore_list's is_undo
 * parameter comment).
 */
static void restore_active_sculpt_layer(StepData &step_data, Object &object, const bool is_undo)
{
  if (step_data.sculpt_layer_uid == 0 || step_data.sculpt_layer_data.is_empty()) {
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  SculptLayer *layer = sculpt_layer_find(mesh, step_data.sculpt_layer_uid);
  if (!layer || !layer->data) {
    return;
  }
  MutableSpan<float3> live(static_cast<float3 *>(layer->data), layer->totelem);
  MutableSpan<float3> stored = step_data.sculpt_layer_data;
  if (step_data.sculpt_layer_grids) {
    /* Multires/grid path: per-grid tangent-space deltas at the touched grids. Only the layer
     * data is adjusted here; the CCG positions are re-evaluated from base + layers by the
     * caller (the composed surface is a deterministic function of the stored data). */
    const Span<int> grids = step_data.sculpt_layer_verts;
    if (grids.is_empty() || stored.size() % grids.size() != 0) {
      return;
    }
    const int64_t grid_area = stored.size() / grids.size();
    const bool undo_grids = is_undo;
    for (const int64_t t : grids.index_range()) {
      const int64_t start = int64_t(grids[t]) * grid_area;
      if (start < 0 || start + grid_area > live.size()) {
        continue;
      }
      float3 *layer_grid = live.data() + start;
      const float3 *delta_grid = stored.data() + t * grid_area;
      for (int64_t i = 0; i < grid_area; i++) {
        if (undo_grids) {
          layer_grid[i] -= delta_grid[i];
        }
        else {
          layer_grid[i] += delta_grid[i];
        }
      }
    }
    return;
  }
  /* Partial delta snapshot (mesh path): add or subtract per-vertex deltas.
   * #sculpt_layer_data holds the delta (new - old) recorded at stroke end; undo subtracts it,
   * redo adds it. Unlike the whole-buffer path the delta is constant, so no swap is needed.
   *
   * The arrays may carry per-node holes (see #StepData::sculpt_layer_seg_start): when a segment
   * table is present only the listed ranges are valid, otherwise the arrays are tightly packed. */
  const Span<int> verts = step_data.sculpt_layer_verts;
  if (verts.size() != stored.size()) {
    return;
  }
  const bool undo = is_undo;
  /* Deliberately the CURRENT effective influence, not the one in force when the stroke was
   * recorded: every influence change keeps `positions == base + sum(data * effective)` (canonical
   * recompute or the incremental RNA delta), so the positions embed the recorded delta scaled by
   * the influence in force NOW, and that is the amount to add or remove. */
  const float influence = bke::sculpt_layers::effective(*layer);
  MutableSpan<float3> positions = mesh.vert_positions_for_write();

  /* Under a shape key the combined surface is recomposed from the base plus layers at evaluation
   * (the layer overlay is applied on top of the shape-keyed positions), and #mesh.vert_positions
   * holds the untouched basis. Only revert the layer data here; leave the basis alone and let the
   * re-evaluation reflect the change, mirroring how the stroke itself never touched the basis. */
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bool skip_positions = ss.shapekey_active != nullptr;

  const auto apply_range = [&](const int start, const int count) {
    for (const int64_t i : IndexRange(start, count)) {
      const int v = verts[i];
      if (v >= 0 && v < live.size()) {
        if (undo) {
          live[v] -= stored[i];
          if (!skip_positions) {
            positions[v] -= stored[i] * influence;
          }
        }
        else {
          live[v] += stored[i];
          if (!skip_positions) {
            positions[v] += stored[i] * influence;
          }
        }
      }
    }
  };
  if (step_data.sculpt_layer_seg_start.is_empty()) {
    /* Legacy tightly-packed layout. */
    apply_range(0, int(verts.size()));
  }
  else {
    for (const int s : step_data.sculpt_layer_seg_start.index_range()) {
      apply_range(step_data.sculpt_layer_seg_start[s], step_data.sculpt_layer_seg_count[s]);
    }
  }
}

void store_active_sculpt_layer_grids(Object &object, Vector<int> &&grids, Vector<float3> &&deltas)
{
  StepData *step_data = get_step_data(object);
  if (!step_data || grids.is_empty() || deltas.is_empty()) {
    return;
  }
  BLI_assert(deltas.size() % grids.size() == 0);
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh);
  if (!layer) {
    return;
  }
  step_data->sculpt_layer_uid = layer->base.uid;
  step_data->sculpt_layer_grids = true;
  step_data->undo_size += grids.as_span().size_in_bytes() + deltas.as_span().size_in_bytes();
  step_data->sculpt_layer_verts = std::move(grids);
  step_data->sculpt_layer_data = std::move(deltas);
}

void store_active_sculpt_layer_verts(Object &object,
                                     Vector<int> &&verts,
                                     Vector<float3> &&deltas,
                                     Vector<int> &&seg_start,
                                     Vector<int> &&seg_count)
{
  StepData *step_data = get_step_data(object);
  if (!step_data || verts.is_empty()) {
    return;
  }
  BLI_assert(verts.size() == deltas.size());
  BLI_assert(seg_start.size() == seg_count.size());
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  SculptLayer *layer = bke::sculpt_layers::active_get(mesh);
  if (!layer) {
    return;
  }
  step_data->sculpt_layer_uid = layer->base.uid;
  step_data->undo_size += verts.as_span().size_in_bytes() + deltas.as_span().size_in_bytes() +
                          seg_start.as_span().size_in_bytes() + seg_count.as_span().size_in_bytes();
  step_data->sculpt_layer_verts = std::move(verts);
  step_data->sculpt_layer_data = std::move(deltas);
  step_data->sculpt_layer_seg_start = std::move(seg_start);
  step_data->sculpt_layer_seg_count = std::move(seg_count);
}

SculptLayerUndoPayload::SculptLayerUndoPayload(SculptLayerUndoPayload &&other) noexcept
    : type(other.type),
      name(std::move(other.name)),
      flag(other.flag),
      color_tag(other.color_tag),
      uid(other.uid),
      parent_uid(other.parent_uid),
      prev_uid(other.prev_uid),
      influence(other.influence),
      totelem(other.totelem),
      domain(other.domain),
      level(other.level),
      data(other.data),
      mask(other.mask)
{
  other.data = nullptr;
  other.mask = nullptr;
}

SculptLayerUndoPayload &SculptLayerUndoPayload::operator=(SculptLayerUndoPayload &&other) noexcept
{
  if (this != &other) {
    if (data) {
      MEM_delete_void(data);
    }
    bke::sculpt_layers::mask_free(mask);
    type = other.type;
    name = std::move(other.name);
    flag = other.flag;
    color_tag = other.color_tag;
    uid = other.uid;
    parent_uid = other.parent_uid;
    prev_uid = other.prev_uid;
    influence = other.influence;
    totelem = other.totelem;
    domain = other.domain;
    level = other.level;
    data = other.data;
    mask = other.mask;
    other.data = nullptr;
    other.mask = nullptr;
  }
  return *this;
}

SculptLayerUndoPayload::~SculptLayerUndoPayload()
{
  /* A folder payload never acquires a buffer — nothing but the layer branch of
   * #sculpt_layer_payload_capture / #sculpt_layer_payload_extract ever writes #data — so the free
   * below is unreachable for one. Asserted rather than assumed: a future path that filled #data
   * without setting #type would hand a #SculptLayerGroup's bytes to #MEM_delete_void. */
  BLI_assert(this->is_layer() || data == nullptr);
  if (data) {
    MEM_delete_void(data);
  }
  /* No kind test, unlike #data: a folder carries a mask exactly as a layer does, and both payload
   * kinds may therefore own one. A no-op when the payload never held a mask, or handed it back to
   * the tree in #sculpt_layer_payload_insert. */
  bke::sculpt_layers::mask_free(mask);
}

/* Everything a payload mirrors from a node except its slot in the tree, whose meaning differs per
 * direction, and a layer's data buffer, whose ownership does. Kept in one place so that a new node
 * field cannot be handled on one of the payload paths and forgotten on the others.
 *
 * The layer-only fields are filled only for a layer, which is what keeps a folder payload's copy of
 * them at the defaults #SculptLayerUndoPayload documents. */
static void payload_metadata_from_node(SculptLayerUndoPayload &payload,
                                       const SculptLayerTreeNode &node)
{
  payload.type = node.type;
  payload.name = node.name;
  payload.flag = node.flag;
  payload.color_tag = node.color_tag;
  payload.uid = node.uid;
  payload.sync_uid = node.sync_uid;
  /* The parent is a pointer now rather than a stored uid, so it is read off the tree here. Null only
   * for the root group, which is never captured; 0 then means "the root folder", which is where
   * #sculpt_layer_payload_insert puts a node back. */
  payload.parent_uid = node.parent ? node.parent->base.uid : 0;
  if (const SculptLayer *layer = bke::sculpt_layers::node_as_layer(&node)) {
    payload.influence = layer->influence;
    payload.totelem = layer->totelem;
    payload.domain = layer->domain;
    payload.level = layer->level;
  }
}

/* The inverse of #payload_metadata_from_node, onto a node the caller allocated for `payload.type`.
 *
 * #SculptLayerTreeNode::parent and the sibling links are deliberately not written: linking the node
 * is what sets them (see #sculpt_layer_payload_insert), and a parent pointer written without the
 * matching entry in that parent's child list would be a tree that disagrees with itself. */
static void node_metadata_from_payload(SculptLayerTreeNode &node,
                                       const SculptLayerUndoPayload &payload)
{
  /* Before the kind test below: #SculptLayerTreeNode::type defaults to the layer type, so a folder
   * rebuilt from a payload would otherwise claim to be a layer. */
  node.type = payload.type;
  /* Safe against truncation on repeated round trips: `payload.name` was itself read out of a
   * fixed-size #SculptLayerTreeNode::name (see #payload_metadata_from_node), so it can never exceed
   * what this copy back can hold. */
  STRNCPY_UTF8(node.name, payload.name.c_str());
  node.flag = payload.flag;
  node.color_tag = payload.color_tag;
  node.uid = payload.uid;
  node.sync_uid = payload.sync_uid;
  if (SculptLayer *layer = bke::sculpt_layers::node_as_layer(&node)) {
    layer->influence = payload.influence;
    layer->totelem = payload.totelem;
    layer->domain = payload.domain;
    layer->level = payload.level;
  }
}

/* The folder a recorded `parent_uid` / #ReparentMove::group_to names, falling back to the root.
 *
 * Unlike #sculpt_layer_find, uid 0 is *not* guarded away here: the root group holds uid 0, so
 * "recorded at the top level" and "resolves to the root" are the same lookup, which is exactly why
 * a payload can store the parent as a plain uid with no separate "was at the root" marker. */
static SculptLayerGroup *sculpt_layer_parent_resolve(Mesh &mesh, const int parent_uid)
{
  SculptLayerGroup *parent = bke::sculpt_layers::node_as_group(
      bke::sculpt_layers::node_find_by_uid(mesh, parent_uid));
  if (parent == nullptr) {
    /* The folder is gone (removed by a later step that is not being undone), or the uid names a
     * layer. The root is the closest surviving approximation; leaving the node unlinked would leak
     * it and drop it off the tree entirely. */
    CLOG_WARN(&LOG,
              "Sculpt layer undo: folder %d recorded as a parent is missing; falling back to the "
              "root folder",
              parent_uid);
    parent = bke::sculpt_layers::root_group(mesh);
  }
  return parent;
}

/* The sibling \a node must land after inside \a parent for a recorded \a prev_uid, or null for the
 * head. \a node may be null when nothing is being re-linked yet.
 *
 * The anchor may be of either kind: one uid space means a folder and a layer are ordinary siblings
 * in #SculptLayerGroup::children, so the lookup is over nodes and no kind test applies. */
static SculptLayerTreeNode *sculpt_layer_anchor_resolve(Mesh &mesh,
                                                        const int prev_uid,
                                                        const SculptLayerGroup &parent,
                                                        const SculptLayerTreeNode *node)
{
  if (prev_uid == 0) {
    /* The head. Not looked up: #node_find_by_uid resolves uid 0 to the root folder, which is never
     * anyone's sibling. */
    return nullptr;
  }
  SculptLayerTreeNode *after = bke::sculpt_layers::node_find_by_uid(mesh, prev_uid);
  if (after == nullptr || after == node || after->parent != &parent) {
    /* Either the anchor is gone (removed by a later step that is not being undone), or it no longer
     * sits in the folder the node is going into, in which case #node_move_into could not insert
     * after it at all. The head is the closest surviving approximation of where the node sat. */
    return nullptr;
  }
  return after;
}

/* Whether \a node may legally be moved into \a dst.
 *
 * Only a folder can fail this: putting one inside itself or its own subtree would detach that
 * subtree from the root and leak it. #node_move_into asserts the two cases rather than handling
 * them, so a recorded move is checked here first — the tree can have changed since the move was
 * captured (a later step that is not being undone), which is precisely what a debug-only assert
 * cannot cover in a release build. */
static bool sculpt_layer_move_is_legal(const SculptLayerTreeNode &node,
                                       const SculptLayerGroup &dst)
{
  const SculptLayerGroup *moved = bke::sculpt_layers::node_as_group(&node);
  if (moved == nullptr) {
    return true;
  }
  return &dst != moved && !bke::sculpt_layers::node_is_descendant_of(dst.base, *moved);
}

SculptLayerUndoPayload sculpt_layer_payload_capture(Mesh & /*mesh*/,
                                                    SculptLayerTreeNode &node,
                                                    const PayloadCapture capture)
{
  SculptLayerUndoPayload payload;
  payload_metadata_from_node(payload, node);
  /* Read while the node is still linked; the caller unlinks it afterwards. */
  payload.prev_uid = node.prev ? node.prev->uid : 0;
  /* Only a layer owns a buffer. This branch is the whole reason a folder payload can never reach the
   * free in #SculptLayerUndoPayload::~SculptLayerUndoPayload. */
  if (SculptLayer *layer = bke::sculpt_layers::node_as_layer(&node)) {
    payload.data = layer->data;
    layer->data = nullptr;
    layer->totelem = 0;
  }
  /* Taken from both kinds, and only when the node is on its way out: #bke::sculpt_layers::remove and
   * #group_remove free the node's mask along with the node, so the transfer has to happen here or
   * the weights are gone before the payload ever sees them. A #PayloadCapture::DataOnly capture
   * leaves the node in the tree, where its mask stays valid and untouched. */
  if (capture == PayloadCapture::NodeRemoved) {
    payload.mask = node.mask;
    node.mask = nullptr;
  }
  return payload;
}

void sculpt_layer_payload_insert(Mesh &mesh, SculptLayerUndoPayload &payload)
{
  SculptLayerTreeNode *node = nullptr;
  if (payload.is_layer()) {
    SculptLayer *layer = MEM_new<SculptLayer>(__func__);
    node_metadata_from_payload(layer->base, payload);
    layer->data = payload.data;
    payload.data = nullptr;
    node = &layer->base;
  }
  else {
    SculptLayerGroup *group = MEM_new<SculptLayerGroup>(__func__);
    node_metadata_from_payload(group->base, payload);
    /* A folder coming into existence outside the module's own creation paths — this is one of the
     * two, next to the blend-file reader — must be given its runtime here: #bke::sculpt_layers::
     * layers dereferences it rather than allocating it on demand (it is const and runs from
     * evaluation threads), so without this the next span rebuild of any folder above this one
     * dereferences null. */
    bke::sculpt_layers::group_runtime_ensure(*group);
    node = &group->base;
  }

  /* The mask goes back with the node, for both kinds. Without this a masked node that was deleted
   * would come back unmasked: the weights are owned by the payload from the capture onwards (see
   * #sculpt_layer_payload_capture), and nothing else would ever hand them to the tree again.
   * Assigned rather than swapped — the node was allocated a few lines above and has no mask of its
   * own to displace. */
  BLI_assert(node->mask == nullptr);
  node->mask = payload.mask;
  payload.mask = nullptr;

  /* Through the module's own move rather than a raw #BLI_addhead / #BLI_insertlinkafter: it is what
   * maintains #SculptLayerTreeNode::parent and tags the cached layer spans of both ends dirty, and a
   * missed tag hands the eval paths a pointer to a freed node. The node was just allocated and has
   * no parent to unlink from, which #node_move_into's null-parent branch covers. */
  SculptLayerGroup &parent = *sculpt_layer_parent_resolve(mesh, payload.parent_uid);
  bke::sculpt_layers::node_move_into(
      mesh, *node, parent, sculpt_layer_anchor_resolve(mesh, payload.prev_uid, parent, node));
}

/* Pull the node identified by `payload.uid` out of the tree back into \a payload, transferring a
 * layer's data buffer to the undo step. Returns false when the node is missing.
 *
 * A folder must already be empty: it owns its children, so this refuses to guess what an orphaned
 * subtree should become (#bke::sculpt_layers::group_remove asserts it). The operator records the
 * lift-out as a reparent batch, which #restore_list re-applies before it gets here. */
static bool sculpt_layer_payload_extract(Mesh &mesh, SculptLayerUndoPayload &payload)
{
  if (payload.uid == 0) {
    /* Uid 0 is the root folder, which is never captured into a payload and must never be freed by
     * one. An uninitialized payload reaching here is a bug elsewhere, not a request to extract it. */
    BLI_assert_unreachable();
    return false;
  }
  SculptLayerTreeNode *node = bke::sculpt_layers::node_find_by_uid(mesh, payload.uid);
  if (node == nullptr) {
    return false;
  }
  /* Nothing may already be owned here: a payload is extracted into only after it was inserted from
   * (which nulls the pointer) or while it holds nothing but a uid. */
  BLI_assert(payload.data == nullptr);
  BLI_assert(payload.mask == nullptr);
  payload_metadata_from_node(payload, *node);
  payload.prev_uid = node->prev ? node->prev->uid : 0;

  /* Before either removal below, which free the node's mask along with the node. Taken for both
   * kinds, so a folder's mask survives an undo/redo cycle exactly as a layer's does. */
  payload.mask = node->mask;
  node->mask = nullptr;

  if (SculptLayer *layer = bke::sculpt_layers::node_as_layer(node)) {
    payload.data = layer->data;
    layer->data = nullptr;
    /* Hands the active marker to a sibling layer when this layer held it. */
    bke::sculpt_layers::remove(mesh, *layer);
    return true;
  }
  SculptLayerGroup *group = bke::sculpt_layers::node_as_group(node);
  /* A node is of one kind or the other, and the layer branch above returned. */
  BLI_assert(group != nullptr);
  bke::sculpt_layers::group_remove(mesh, *group);
  return true;
}

void push_sculpt_layer_list_change(Object &object,
                                   Vector<SculptLayerUndoPayload> &&removed,
                                   Vector<int> &&added_uids,
                                   const bool is_bake)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.removed = std::move(removed);
  /* Charged after the move, so the payloads counted are the ones the step now owns. Only the mask is
   * added here: #SculptLayerUndoPayload::data is the layer's own buffer, whose accounting is not
   * this change's to introduce. */
  for (const SculptLayerUndoPayload &payload : step_data->sculpt_layer_op.removed) {
    if (payload.mask != nullptr) {
      step_data->undo_size += size_t(bke::sculpt_layers::mask_size_in_bytes(*payload.mask));
    }
  }
  step_data->sculpt_layer_op.added.clear();
  for (const int uid : added_uids) {
    SculptLayerUndoPayload payload;
    payload.uid = uid;
    /* Explicit rather than left at the (layer) default, for the same reason
     * #push_sculpt_layer_group_list_change spells the folder type out. */
    payload.type = SCULPT_LAYER_TREE_NODE_TYPE_LAYER;
    step_data->sculpt_layer_op.added.append(std::move(payload));
  }
  step_data->sculpt_layer_op.is_bake = is_bake;
}

void push_sculpt_layer_bake_shape_key(Object &object, const int key_uid)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->sculpt_layer_op.bake_key_uid = key_uid;
}

void push_sculpt_layer_bake_to_shape_key(Object &object, const short pre_bake_shapenr)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->sculpt_layer_op.created_key = true;
  step_data->sculpt_layer_op.pre_bake_shapenr = pre_bake_shapenr;
}

void push_sculpt_layer_reparent(Object &object, Vector<ReparentMove> &&moves)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.moves = std::move(moves);
}

void push_sculpt_layer_active(Object &object, const int uid_from, const int uid_to)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.active_uid_from = uid_from;
  step_data->sculpt_layer_op.active_uid_to = uid_to;
  step_data->sculpt_layer_op.has_active_change = true;
}

void push_sculpt_layer_metadata(Object &object, const SculptLayerTreeNode &node)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  /* The root group (uid 0) has no metadata worth recording — it is never renamed, never re-flagged
   * and never drawn — and #restore_list guards its swap behind `op.uid != 0`, so a root passed here
   * would be captured only to be silently skipped on restore. No caller does this today; the assert
   * is the contract, since neither the signature nor a runtime check would otherwise catch it. */
  BLI_assert(node.uid != 0);
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.uid = node.uid;
  step_data->sculpt_layer_op.flag = node.flag;
  step_data->sculpt_layer_op.color_tag = node.color_tag;
  /* Always read out of the fixed-size #SculptLayerTreeNode::name, never from a caller-supplied
   * string, so the stored copy can never exceed what the swap on restore can write back — otherwise
   * a long name would be truncated a little more on each undo/redo round trip. */
  step_data->sculpt_layer_op.name = node.name;
  /* Influence is a layer field: a folder has none, and the restore's swap of it is skipped for one.
   * Left at its default here rather than swapped with a meaningless value. */
  if (const SculptLayer *layer = bke::sculpt_layers::node_as_layer(&node)) {
    step_data->sculpt_layer_op.influence = layer->influence;
  }
  step_data->sculpt_layer_op.has_data = false;
}

void push_sculpt_layer_group_list_change(Object &object,
                                         Vector<SculptLayerUndoPayload> &&removed,
                                         Vector<int> &&added_uids)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.groups_removed = std::move(removed);
  step_data->sculpt_layer_op.groups_added.clear();
  for (const int uid : added_uids) {
    SculptLayerUndoPayload payload;
    payload.uid = uid;
    /* Explicit rather than left at the default: the payload is filled in for real only when a later
     * undo extracts the folder into it, and until then #SculptLayerUndoPayload::is_layer would claim
     * a folder is a layer. */
    payload.type = SCULPT_LAYER_TREE_NODE_TYPE_GROUP;
    step_data->sculpt_layer_op.groups_added.append(std::move(payload));
  }
}

void push_sculpt_layer_flags_batch(Object &object, Vector<int> &&uids, Vector<int> &&flags)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  BLI_assert(uids.size() == flags.size());
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.flags_batch_uids = std::move(uids);
  step_data->sculpt_layer_op.flags_batch_flags = std::move(flags);
}

void push_sculpt_layer_data(Object &object, const SculptLayer &layer)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.uid = layer.base.uid;
  step_data->sculpt_layer_op.influence = layer.influence;
  step_data->sculpt_layer_op.flag = layer.base.flag;
  step_data->sculpt_layer_op.name = layer.base.name;
  if (layer.data && layer.totelem > 0) {
    const Span<float3> src(static_cast<const float3 *>(layer.data), layer.totelem);
    step_data->sculpt_layer_op.data = Array<float3>(src);
    step_data->sculpt_layer_op.has_data = true;
  }
  else {
    step_data->sculpt_layer_op.has_data = false;
  }
}

void push_sculpt_layer_mask(Object &object, const SculptLayerTreeNode &node)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  /* Uid 0 is the root group, whose mask is never editable — the restore's `mask_uid != 0` test would
   * skip it anyway, so capturing one would only snapshot a mask that can never be put back. */
  BLI_assert(node.uid != 0);
  /* Refused, loudly, while a weight-mask editing session is open on this very node: the node's
   * weights then live in the dense standard mask storage and #SculptLayerTreeNode::mask holds the
   * stale value it had when the session opened. Snapshotting that would capture a mask the user
   * never made, and the restore's swap would write it into a field the next #layers::mask_edit_end
   * unconditionally overwrites from the dense buffer — an undo that appears to do nothing at all.
   *
   * The caller's contract, not a condition to recover from: an operator that edits the mask of the
   * node being edited must go through the session's dense buffer, which is where the authoritative
   * weights are (see the constraint recorded on this function in `sculpt_undo.hh`). Asserted so a
   * debug build stops at the offending call site, and logged as an error so a release build cannot
   * lose the edit quietly. */
  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss != nullptr && ss->layers.mask_edit.node_uid == node.uid) {
    BLI_assert_unreachable();
    CLOG_ERROR(&LOG,
               "Sculpt layer mask undo: refusing to capture node %d while its weight-mask editing "
               "session is open; the capture would snapshot a stale mask",
               node.uid);
    return;
  }
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.mask_uid = node.uid;
  /* A copy rather than the node's own pointer: the operator goes on to edit or replace the live
   * mask, and taking it here would leave the node unmasked in the state the user is looking at.
   * #SculptLayerUndoPayload takes the pointer instead because there the node is going away. */
  if (step_data->sculpt_layer_op.mask_swap != nullptr) {
    /* Uncharged again before being replaced, so a step that captures twice is not billed twice. */
    step_data->undo_size -= size_t(
        bke::sculpt_layers::mask_size_in_bytes(*step_data->sculpt_layer_op.mask_swap));
  }
  bke::sculpt_layers::mask_free(step_data->sculpt_layer_op.mask_swap);
  step_data->sculpt_layer_op.mask_swap = node.mask ? bke::sculpt_layers::mask_copy(*node.mask) :
                                                     nullptr;
  /* Charged against the undo memory limit: without this a run of mask edits on a dense mesh reads
   * as free and the stack is never trimmed for it. */
  if (step_data->sculpt_layer_op.mask_swap != nullptr) {
    step_data->undo_size += size_t(
        bke::sculpt_layers::mask_size_in_bytes(*step_data->sculpt_layer_op.mask_swap));
  }
}

void push_sculpt_layer_mask_session(Object &object, const int node_uid, const bool entering)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  /* Uid 0 is both the root group and the "no session" sentinel, so it can never name a session. */
  BLI_assert(node_uid != 0);
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.mask_session_uid = node_uid;
  step_data->sculpt_layer_op.mask_session_entering = entering;
}

MaskSessionBoundary mask_session_boundary(const int step_session_uid,
                                          const bool step_entering,
                                          const bool is_undo)
{
  MaskSessionBoundary boundary;
  if (step_session_uid == 0) {
    return boundary;
  }
  boundary.node_uid = step_session_uid;
  /* Undo lands on the state before the step and redo on the state after it, so an entering step
   * wants the session open on redo and closed on undo, and an exiting step the other way round. */
  boundary.want_open = is_undo ? !step_entering : step_entering;
  return boundary;
}

void push_sculpt_layer_data_resize(Object &object, Vector<SculptLayerUndoPayload> &&resized)
{
  StepData *step_data = get_step_data(object);
  if (!step_data) {
    return;
  }
  step_data->type = Type::SculptLayer;
  step_data->sculpt_layer_op.resized = std::move(resized);
}

bool foreach_recorded_position_mesh(
    const Object &object, FunctionRef<void(Span<int> verts, Span<float3> orig_positions)> fn)
{
  /* Per-object lookup, never a step-wide one: #Node.vert_indices indexes the vertex array of the
   * object its #StepData belongs to, so folding another participating object's nodes in here would
   * scatter deltas at foreign indices. */
  StepData *step_data = get_step_data(object);
  if (!step_data || step_data->type != Type::Position) {
    return false;
  }

  /* Must run before #push_end, while the per-node undo data is still in the map. */
  for (const std::unique_ptr<Node> &unode : step_data->undo_nodes_by_pbvh_node.values()) {
    /* Skip multires (grid) nodes: this path handles the mesh (vertex) domain only. */
    if (unode->vert_indices.is_empty()) {
      continue;
    }
    const int unique_num = unode->unique_verts_num;
    if (unique_num <= 0) {
      continue;
    }

    /* Prefer #orig_position (original mesh space) when deform modifiers are active, since
     * #position is then stored in evaluated space; otherwise the two are identical. This keeps the
     * delta in the same space as the layer data and the live mesh positions. */
    const Span<float3> orig = !unode->orig_position.is_empty() ?
                                  unode->orig_position.as_span().take_front(unique_num) :
                                  unode->position.as_span().take_front(unique_num);
    fn(unode->vert_indices.as_span().take_front(unique_num), orig);
  }

  return true;
}

bool foreach_recorded_eval_position_mesh(
    const Object &object, FunctionRef<void(Span<int> verts, Span<float3> eval_positions)> fn)
{
  /* Per-object lookup, for the same reason as #foreach_recorded_position_mesh. */
  StepData *step_data = get_step_data(object);
  if (!step_data || step_data->type != Type::Position) {
    return false;
  }

  /* Must run before #push_end, while the per-node undo data is still in the map. */
  for (const std::unique_ptr<Node> &unode : step_data->undo_nodes_by_pbvh_node.values()) {
    if (unode->vert_indices.is_empty()) {
      continue;
    }
    const int unique_num = unode->unique_verts_num;
    if (unique_num <= 0) {
      continue;
    }
    /* Always the evaluated/display positions (#Node.position), unlike #foreach_recorded_position_mesh
     * which prefers #orig_position (base space) when a deform is active. Under a shape key the layer
     * stroke is captured in the evaluated space, so the recorder diffs these against the post-stroke
     * #deform_cos to recover the object-space per-vertex layer delta. */
    fn(unode->vert_indices.as_span().take_front(unique_num),
       unode->position.as_span().take_front(unique_num));
  }

  return true;
}

bool foreach_recorded_grids(const Object &object, FunctionRef<void(Span<int> grids)> fn)
{
  /* Per-object lookup, for the same reason as #foreach_recorded_position_mesh: #Node.grids indexes
   * the #SubdivCCG of the object its #StepData belongs to. */
  StepData *step_data = get_step_data(object);
  if (!step_data || step_data->type != Type::Position) {
    return false;
  }

  /* Must run before #push_end, while the per-node undo data is still in the map. */
  for (const std::unique_ptr<Node> &unode : step_data->undo_nodes_by_pbvh_node.values()) {
    if (unode->grids.is_empty()) {
      continue;
    }
    fn(unode->grids.as_span());
  }

  return true;
}

/* Bring the open weight-mask editing session in line with \a boundary: close whatever is open when
 * the boundary names a different node or none at all, then open the one it asks for.
 *
 * Split from #mask_session_boundary so that the *decision* stays pure and testable and only its
 * effect lives here, and split from #restore_list so that the ordering rule the caller states has
 * one place to point at. */
static void restore_mask_session(bContext *C,
                                 Depsgraph *depsgraph,
                                 Object &object,
                                 const MaskSessionBoundary &boundary)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const int open_uid = ss.layers.mask_edit.node_uid;
  const int wanted_uid = boundary.want_open ? boundary.node_uid : 0;
  if (open_uid == wanted_uid) {
    return;
  }
  if (open_uid != 0) {
    /* Closing compresses the dense weights back onto the node it was opened on. At this point the
     * #Type::Mask steps made inside the session have already been restored (undo walks newest
     * first), so what is compressed is the mask as it stood when the session opened — which is
     * exactly what a later redo of this step expands again. */
    layers::mask_edit_end(object);
  }
  if (wanted_uid == 0) {
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(object.data);
  SculptLayerTreeNode *node = bke::sculpt_layers::node_find_by_uid(mesh, wanted_uid);
  if (node == nullptr) {
    /* Gone, removed by a later step that is not being undone. There is no mask to edit, and leaving
     * the session closed is the only state the rest of the restore can be consistent with. */
    return;
  }
  /* #mask_edit_enter, not #mask_edit_begin, and this is load-bearing rather than tidiness. REC is
   * live session state that no undo step captures (see the #rec_exemption_refresh note below), so
   * decoding a step cannot assume it stands where it did when that step was recorded. The reachable
   * case: the user closes a session, arms REC — allowed, since the refusal in #layer_toggle_rec_exec
   * only guards against arming *over* an open session — then presses Ctrl+Z. #mask_edit_begin
   * refuses outright while REC is armed, so it would silently reopen nothing and leave the undo
   * state disagreeing with the world. The shared entry disarms REC under the #rec_active_set
   * contract first, which is the state the recorded step describes anyway. */
  if (!layers::mask_edit_enter(*depsgraph, *CTX_data_main(C), object, *node)) {
    /* Refused rather than failed — a stroke still recording, a domain the session cannot open on.
     * (Not REC being armed: the entry above disarms it, and that disarm is one-way, here as
     * everywhere else.) The world stays consistent (no session, the user's own mask live); only the
     * redo of a session the user opened by hand is not reproduced, which is a state they can simply
     * re-enter. */
    CLOG_WARN(&LOG,
              "Sculpt layer undo: could not reopen the mask editing session on node %d",
              wanted_uid);
  }
}

/* \a is_undo is the direction the undo system is decoding in, threaded down from the two
 * #step_decode_undo_impl / #step_decode_redo_impl entry points that already know it, by way of
 * #restore_list's per-object loop.
 *
 * Deliberately a parameter rather than #StepData::needs_undo: that flag is flipped only by the
 * dyntopo and geometry restore helpers, and a #Type::SculptLayer step reaches none of them, so for
 * every step this function's layer branch handles it reads a constant `true`. Every direction test
 * built on it would therefore answer "undo" while redoing. */
static void restore_list_object(bContext *C,
                                Depsgraph *depsgraph,
                                StepData &step_data,
                                Object &object,
                                const bool is_undo)
{
  PRF_scope(ProfileCategory::Editor);
  Scene *scene = CTX_data_scene(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);

  if (!object.runtime->sculpt_session) {
    return;
  }

  SculptSession &ss = *object.runtime->sculpt_session;
  /* Called here for its side effect alone — the session hook below needs a tree to already exist,
   * since #layers::mask_edit_begin refuses when #bke::object::pbvh_get hands it null — and
   * deliberately not bound to a reference: reopening a grid session re-evaluates the depsgraph and
   * replaces the tree, which would leave a reference taken here dangling. The one consumer far
   * below takes a fresh one. */
  bke::object::pbvh_ensure(*depsgraph, object);

  /* Restore pivot. */
  ss.pivot_pos = step_data.pivot_pos;
  ss.pivot_rot = step_data.pivot_rot;

  /* Origin Correct: swap the rigid-body secondary's object matrix with the snapshot, same pattern
   * as the position swap below (#restore_position_mesh's #swap_indexed_data) -- applying the
   * stored matrix and storing the object's pre-restore matrix back means the NEXT call (the
   * opposite undo/redo direction) swaps back correctly. Runs unconditionally on
   * #has_object_transform, independent of #step_data.type/#nodes, since a pure rigid-body
   * secondary never has mesh undo nodes pushed for it at all. */
  if (step_data.has_object_transform) {
    const float4x4 to_restore = step_data.object_transform;
    step_data.object_transform = object.object_to_world();
    BKE_object_apply_mat4(&object, to_restore.ptr(), false, true);
    DEG_id_tag_update(&object.id, ID_RECALC_TRANSFORM);
  }

  /* A weight-mask editing session parks the user's sculpt mask and puts the node's own weights into
   * the standard mask storage in its place, so what a #Type::Mask step's dense buffer *means*
   * depends on whether one is open. Restoring across the step that opened or closed a session would
   * therefore write those buffers into the wrong mask and strand the parked one; the session is
   * moved to the side of the boundary this restore lands on before anything else runs.
   *
   * Placed at the common entry rather than inside the #Type::SculptLayer branch below — the only
   * branch a session step can reach — because closing a session compresses the node's mask and
   * recomposes the surface, which must happen before that branch derives the runtime base from the
   * state it finds. It is the same ordering discipline the folder payloads there already require.
   *
   * Ordered after the Origin Correct swap above because the two are independent (object matrix vs.
   * mask storage) and reopening a session re-evaluates the depsgraph: applying the matrix first
   * lets that evaluation see the transform this restore lands on rather than the previous one. */
  if (step_data.sculpt_layer_op.mask_session_uid != 0) {
    restore_mask_session(C,
                         depsgraph,
                         object,
                         mask_session_boundary(step_data.sculpt_layer_op.mask_session_uid,
                                               step_data.sculpt_layer_op.mask_session_entering,
                                               is_undo));
  }

  if (bmesh_restore(C, *depsgraph, step_data, object)) {
    return;
  }

  /* Switching to sculpt mode does not push a particular type.
   * See #124484. */
  /* TODO: Add explicit type for switching into Sculpt Mode. */
  if (step_data.type == Type::None && step_data.nodes.is_empty()) {
    return;
  }

  /* Operator-level sculpt-layer undo: metadata / data / layer-list toggles. Runs the same code
   * for undo and redo (each sub-operation is its own inverse or direction-aware; the direction is
   * the \a is_undo parameter). */
  if (step_data.type == Type::SculptLayer) {
    Mesh &mesh = *id_cast<Mesh *>(object.data);
    StepData::SculptLayerOpUndo &op = step_data.sculpt_layer_op;

    /* Active-layer selection: pure UI state, no geometry or layer contents involved. Handled
     * before the base derivation / multires flush below, and returns early so undoing a
     * selection never pays a canonical position recompute. */
    if (op.has_active_change) {
      mesh.sculpt_layers_active_uid = is_undo ? op.active_uid_from : op.active_uid_to;
      /* The overlay shows the active node's weight mask, so a different node is now on screen and
       * no node of the draw cache holds a valid value. */
      layers::tag_layer_overlays_dirty(object);
      WM_event_add_notifier(C, NC_GEOM | ND_DATA, &mesh.id);
      return;
    }

    /* A previously restored base-stroke step may have left the CCG marked dirty; consume it
     * before the layer set or influences change, otherwise the later flush would reshape the
     * stale composed CCG against the changed layers and leak the difference into the base. */
    layers::flush_pending_multires_base(object);

    /* The mesh (vertex) domain recomputes canonical positions from the runtime base after the
     * change; derive the base from the current, still-consistent state BEFORE the layer list or
     * metadata is modified. */
    layers::session_state_ensure(object);

    /* Metadata / data swap for the referenced node — of either kind (a layer's influence,
     * visibility, clear / invert and the surviving layer of a merge; a folder's rename and
     * visibility toggle). One uid space means one lookup answers for both, so the folder swap that
     * used to sit in its own branch against its own uid field is this same code now.
     *
     * The 0 test is what keeps uid 0 ("nothing captured" here) away from
     * #bke::sculpt_layers::node_find_by_uid, where it resolves to the root folder. */
    if (op.uid != 0) {
      if (SculptLayerTreeNode *node = bke::sculpt_layers::node_find_by_uid(mesh, op.uid)) {
        std::swap(node->flag, op.flag);
        /* #SCULPT_LAYER_GROUP_MASK_DISABLED rides in the word just swapped, and it decides whether
         * this folder's own mask is folded into the cached chain product. Without this the bit
         * would be restored while the cache kept the product built under the other state, and the
         * recompose below would run against a mask the tree no longer describes.
         *
         * Unconditional rather than gated on a bit difference: the tag is cheap and idempotent, and
         * testing one bit here would put knowledge of which bits exist into a branch whose whole
         * design is to swap the word wholesale. A layer needs nothing — its mask is read straight
         * off the node and caches nowhere — which is why this is scoped to folders, exactly as the
         * matching invalidation in the mask branch below is. */
        if (SculptLayerGroup *group = bke::sculpt_layers::node_as_group(node)) {
          bke::sculpt_layers::tag_chain_mask_dirty(*group);
        }
        std::swap(node->color_tag, op.color_tag);
        std::string live_name = node->name;
        STRNCPY_UTF8(node->name, op.name.c_str());
        op.name = std::move(live_name);
        /* Influence and the data buffer are layer fields. A folder capture left both at their
         * defaults, so there is nothing to swap them with — and reading a folder as a layer would
         * put #SculptLayer::influence over its child list. */
        if (SculptLayer *layer = bke::sculpt_layers::node_as_layer(node)) {
          std::swap(layer->influence, op.influence);
          if (op.has_data && layer->data) {
            MutableSpan<float3> cur(static_cast<float3 *>(layer->data), layer->totelem);
            /* All-or-nothing: a size mismatch means the layer was resized between capture and
             * restore (stale layer / out-of-band edit). A partial swap would leave the buffer as
             * an inconsistent mix of both states, which no further undo/redo could repair. */
            if (cur.size() == op.data.size()) {
              for (int64_t i = 0; i < cur.size(); i++) {
                std::swap(cur[i], op.data[i]);
              }
            }
            else {
              CLOG_WARN(&LOG,
                        "Sculpt layer data size changed since the undo step was captured "
                        "(%d vs %d); skipping the data restore",
                        int(cur.size()),
                        int(op.data.size()));
            }
          }
        }
      }
    }

    /* Weight mask of the referenced node — of either kind. Swapped with the stored one exactly as
     * the metadata above is, which makes it its own inverse and needs no direction test. Kept in its
     * own block against its own uid because a mask operator changes no metadata: sharing #uid would
     * make the swap above write this step's empty name onto the node.
     *
     * A null on either side is a legal state and simply means "no mask", so the swap handles a mask
     * being added and removed with no special case. */
    if (op.mask_uid != 0) {
      if (SculptLayerTreeNode *node = bke::sculpt_layers::node_find_by_uid(mesh, op.mask_uid)) {
        /* All-or-nothing, mirroring the data guard above: two masks that disagree on the domain
         * they describe cannot both be right, and installing the stored one would put a mask on the
         * node whose block table indexes a topology the object no longer has. Every consumer reads
         * a mask through its own block table, so the mismatch would not fault — it would silently
         * weight the wrong elements.
         *
         * Only comparable when both sides carry a mask. A null on either side is the legal "no
         * mask" state (see below), and there is nothing to measure a lone mask against here: a
         * folder's domain is the object's sculpt domain, which mesh data alone does not resolve. */
        const bool domains_agree = node->mask == nullptr || op.mask_swap == nullptr ||
                                   (node->mask->totelem == op.mask_swap->totelem &&
                                    node->mask->block_size == op.mask_swap->block_size);
        if (domains_agree) {
          std::swap(node->mask, op.mask_swap);
          /* Only a *folder's* mask takes part in a cached chain product (#chain_mask folds in the
           * ancestors' masks alone), so only a folder invalidates anything here — and downward, over
           * its own subtree. A layer's mask is read straight off the node and caches nowhere, which
           * is why this is not #tag_layers_cache_dirty: the tree's shape did not change. */
          if (SculptLayerGroup *group = bke::sculpt_layers::node_as_group(node)) {
            bke::sculpt_layers::tag_chain_mask_dirty(*group);
          }
        }
        else {
          CLOG_WARN(&LOG,
                    "Sculpt layer mask domain changed since the undo step was captured "
                    "(%lld elements at %d vs %lld at %d); skipping the mask restore",
                    (long long)node->mask->totelem,
                    node->mask->block_size,
                    (long long)op.mask_swap->totelem,
                    op.mask_swap->block_size);
        }
      }
    }

    /* Data buffers the operator resized: swap each stored buffer back with the live one. Carries
     * #totelem alongside the pointer, so the two states may differ in size (which is exactly why
     * the in-place swap above cannot express this). */
    for (SculptLayerUndoPayload &payload : op.resized) {
      /* Only a layer has a buffer to resize; #push_sculpt_layer_data_resize is documented for
       * layers alone, and #sculpt_layer_find would hand back null for a folder anyway. */
      BLI_assert(payload.is_layer());
      /* These payloads come from a #PayloadCapture::DataOnly capture: the layer never left the tree
       * and kept its mask, so there is nothing here to swap and a mask would have nowhere to go. */
      BLI_assert(payload.mask == nullptr);
      if (SculptLayer *layer = sculpt_layer_find(mesh, payload.uid)) {
        std::swap(layer->data, payload.data);
        std::swap(layer->totelem, payload.totelem);
      }
    }

    /* Batch flag swap (Solo Base, folder visibility cascade): swap every affected layer's flags
     * with the stored pre-change values. Layers only, both callers included: Solo Base marks
     * layers, and #bke::sculpt_layers::resync_group_state writes #SCULPT_LAYER_GROUP_HIDDEN on
     * layers rather than on the folders that caused it. */
    for (const int64_t i : op.flags_batch_uids.index_range()) {
      if (SculptLayer *layer = sculpt_layer_find(mesh, op.flags_batch_uids[i])) {
        std::swap(layer->base.flag, op.flags_batch_flags[i]);
      }
    }

    /* Immediately after the flag swap above, which is what makes it necessary.
     * #SCULPT_LAYER_REC_EXEMPT rides in the same word, so the swap can just as easily resurrect an
     * exemption captured while REC was armed as drop the one that should be set — and from here on
     * this branch both composes (the bake fold-in / fold-out below calls
     * #bke::sculpt_layers::apply_vert_layer_to_shape_keys and
     * #BKE_multires_sculpt_layer_apply_to_mdisps, both of which resolve the layer's mask through
     * #node_mask_for_composite) and inverts (#layers::session_state_ensure further down). Either one
     * run under a swapped-in bit dents the base by the difference between the masked and the
     * unmasked contribution, permanently.
     *
     * Correct in both directions because it does not read the step at all: it re-derives the answer
     * from the live #SculptSession and the active uid restored at the top of this branch, and REC is
     * session state that no undo step captures. Undo and redo therefore settle on the same answer
     * the live positions were composed under, whatever the swapped word happened to hold. */
    layers::rec_exemption_refresh(object);

    /* Folder tree changes, first half: re-create the folders this direction needs to exist.
     *
     * The folder branch straddles the #moves batch rather than sitting on one side of it, because a
     * folder is now a real container: a node can only be linked into a folder that already exists,
     * and #bke::sculpt_layers::group_remove refuses a folder that still has children (it owns them,
     * so freeing one would leak the subtree). That gives one rule for both directions — insert
     * before the moves, extract after them — and it is what makes the two folder operators reverse:
     *
     * - "Create a folder, move nodes into it" (#SCULPT_OT_layer_group_add) records the folder in
     *   #groups_added and the moves after it. Undoing runs the moves backwards, lifting the nodes
     *   back out, and only then can extract the emptied folder. Redoing re-creates the folder
     *   first, and only then can move the nodes back in.
     * - "Disband a folder, lifting its children out" (#SCULPT_OT_layer_group_remove) records the
     *   lift-out as moves and the folder in #groups_removed, and reverses by the same rule.
     *
     * Within one direction the payloads run in capture order, so each one's anchor is restored
     * before it is, and a folder nested in another is inserted after the folder holding it. */
    if (is_undo) {
      for (SculptLayerUndoPayload &payload : op.groups_removed) {
        sculpt_layer_payload_insert(mesh, payload);
      }
    }
    else {
      for (SculptLayerUndoPayload &payload : op.groups_added) {
        sculpt_layer_payload_insert(mesh, payload);
      }
    }

    /* Node move batch: put each moved node back into the folder it was in and after the sibling it
     * followed in the target state, in capture order — a node's anchor is always restored before it
     * is (mirrors #removed/#added below and #SculptLayerUndoPayload::prev_uid). That anchor may be a
     * folder rather than a layer: they are siblings in one #SculptLayerGroup::children list, and one
     * uid counter spans both, which is why an entry no longer has to say which kind it moves.
     *
     * The active layer is left untouched: reordering never changes which uid is active, and a
     * multi-node drop's moved set is not implicitly "the active layer" — forcing one would silently
     * reassign active from a drag-selection, which is meant to be pure UI state. */
    for (const ReparentMove &move : op.moves) {
      if (move.uid == 0) {
        /* Uid 0 is the root folder, which never moves. A zeroed entry is not a real move. */
        BLI_assert_unreachable();
        continue;
      }
      SculptLayerTreeNode *node = bke::sculpt_layers::node_find_by_uid(mesh, move.uid);
      if (node == nullptr) {
        /* Gone, removed by a later step that is not being undone. */
        continue;
      }
      const int target_prev_uid = is_undo ? move.prev_from : move.prev_to;
      const int target_group_uid = is_undo ? move.group_from : move.group_to;
      SculptLayerGroup &dst = *sculpt_layer_parent_resolve(mesh, target_group_uid);
      if (!sculpt_layer_move_is_legal(*node, dst)) {
        CLOG_WARN(&LOG,
                  "Sculpt layer undo: recorded move of folder %d into %d would nest it in its own "
                  "subtree; skipping",
                  move.uid,
                  target_group_uid);
        continue;
      }
      /* Through the module's own move rather than an open-coded relink: it maintains
       * #SculptLayerTreeNode::parent (which replaced the parent uid tag this branch used to write by
       * hand) and tags the cached layer spans of *both* the source and the destination folder dirty,
       * up to the root. Those spans are what the eval paths read, so a missed tag is a pointer to a
       * freed node rather than a stale row. */
      bke::sculpt_layers::node_move_into(
          mesh, *node, dst, sculpt_layer_anchor_resolve(mesh, target_prev_uid, dst, node));
    }

    /* Folder tree changes, second half: remove the folders this direction no longer needs, now that
     * the moves above have emptied them. See the first half for why the two are split.
     *
     * Reversed relative to the insertion pass: a batch may hold several nested folders (a recursive
     * merge removes a folder together with every folder inside it), and #sculpt_layer_payload_extract
     * refuses a folder that still owns children. The insert pass runs the batch top-down so a nested
     * folder always finds its parent already there; the extract pass therefore has to run it
     * bottom-up, freeing the deepest folder first. For a single-folder batch (#SCULPT_OT_layer_group_add
     * / #SCULPT_OT_layer_group_remove) the two orders coincide, so this is a no-op there. */
    if (is_undo) {
      for (int64_t i = op.groups_added.size() - 1; i >= 0; i--) {
        sculpt_layer_payload_extract(mesh, op.groups_added[i]);
      }
    }
    else {
      for (int64_t i = op.groups_removed.size() - 1; i >= 0; i--) {
        sculpt_layer_payload_extract(mesh, op.groups_removed[i]);
      }
    }

    /* Bake-to-shape-key redo: rebuild the #Key this step created from scratch, using the layers
     * that are still present at this point (the generic layer-list swap below has not run yet —
     * it would otherwise remove them before #bake_vert_layers_into_new_shape_key can sum them).
     * Mirrors the operator's own bootstrap (see #layer_bake_to_shape_key_exec). */
    if (op.created_key && !is_undo) {
      Main *bmain_rw = CTX_data_main(C);
      Key *key = mesh.key = BKE_key_add(bmain_rw, &mesh.id);
      key->type = KEY_RELATIVE;
      KeyBlock *basis = BKE_keyblock_add_ctime(key, DATA_("Basis"), false);
      BKE_keyblock_convert_from_mesh(&mesh, key, basis);
      KeyBlock *baked = bke::sculpt_layers::bake_vert_layers_into_new_shape_key(mesh);
      object.shapenr = BLI_findindex(&key->block, (baked && baked->data) ? baked : basis) + 1;
    }

    /* Layer-list changes: toggle the affected layers between the undo step and the mesh list.
     * `removed` holds layers the operator removed (in the step after the operator ran), `added`
     * holds layers the operator added (in the mesh after the operator ran). Data buffers move by
     * pointer, so this is cheap even for dense meshes. */
    if (is_undo) {
      for (SculptLayerUndoPayload &payload : op.removed) {
        sculpt_layer_payload_insert(mesh, payload);
        if (op.is_bake) {
          /* Undo of a bake: remove this layer's baked contribution from the base again. Which base
           * the bake folded it into depends on the session: MDisps for grids, every key block for
           * absolute shape keys, and for relative shape keys a whole new key block, which is
           * detached below instead (#bake_key_uid). Without shape keys this is a no-op: the live
           * positions carry the layer and the recompute below restores them. */
          if (SculptLayer *layer = sculpt_layer_find(mesh, payload.uid)) {
            const float eff = bke::sculpt_layers::effective(*layer);
            if (layer->domain == SCULPT_LAYER_DOMAIN_GRID) {
              BKE_multires_sculpt_layer_apply_to_mdisps(mesh, *layer, -eff);
            }
            else if (op.bake_key_uid == 0 && !op.created_key) {
              /* Symmetric with the bake across the undo boundary only because
               * #sculpt_layer_payload_insert above put the node's mask back before this runs: both
               * this call and the bake resolve the weights through #node_mask_for_composite, so the
               * same mask that scaled the contribution in scales it back out. Were the layer
               * re-inserted unmasked, the subtraction would leave a permanent `-(1 - mask[i]) *
               * delta` residue in the positions and in every absolute key block —
               * #recompute_mesh_canonical cannot absorb it under shape keys, since it returns early
               * there and re-evaluates instead of recomposing from `mesh_base`. */
              bke::sculpt_layers::apply_vert_layer_to_shape_keys(mesh, *layer, -eff);
            }
          }
        }
      }
      for (SculptLayerUndoPayload &payload : op.added) {
        sculpt_layer_payload_extract(mesh, payload);
      }
    }
    else {
      for (SculptLayerUndoPayload &payload : op.added) {
        sculpt_layer_payload_insert(mesh, payload);
      }
      for (SculptLayerUndoPayload &payload : op.removed) {
        if (op.is_bake) {
          /* Redo of a bake: fold the contribution back into the base before extracting (see the
           * undo branch above for which base that is). */
          if (SculptLayer *layer = sculpt_layer_find(mesh, payload.uid)) {
            const float eff = bke::sculpt_layers::effective(*layer);
            if (layer->domain == SCULPT_LAYER_DOMAIN_GRID) {
              BKE_multires_sculpt_layer_apply_to_mdisps(mesh, *layer, eff);
            }
            else if (op.bake_key_uid == 0 && !op.created_key) {
              bke::sculpt_layers::apply_vert_layer_to_shape_keys(mesh, *layer, eff);
            }
          }
        }
        sculpt_layer_payload_extract(mesh, payload);
      }
    }
    if (op.bake_key_uid != 0 && mesh.key != nullptr) {
      /* Relative shape keys: the bake put the combined layer contribution into a new key block.
       * Undo unlinks it from the mesh (the step keeps it alive until it is freed or redone), redo
       * links it back where it was taken from.
       *
       * The bake appends the block at the tail and nothing is relative to it at that moment, but
       * this code runs arbitrarily later: between the bake and its undo the user may have pointed
       * another block's Relative To at it, moved it (#BKE_keyblock_move) or added blocks around it.
       * Since #KeyBlock::relative is a positional index into #Key::block, the detach and the
       * reattach each have to fix those indices up — the same bookkeeping
       * #BKE_object_shapekey_remove does, mirrored so that undo and redo compose. */
      Key &key = *mesh.key;
      if (is_undo && op.bake_key == nullptr) {
        for (KeyBlock &kb : key.block) {
          if (kb.uid != op.bake_key_uid) {
            continue;
          }
          const int index = BLI_findindex(&key.block, &kb);
          /* The bake never makes its block the reference (it is created relative to the existing
           * one), so the refkey re-seeding half of #BKE_object_shapekey_remove cannot apply here. */
          BLI_assert(key.refkey != &kb);
          op.relative_uids.clear();
          for (KeyBlock &other : key.block) {
            if (&other == &kb) {
              continue;
            }
            if (other.relative == index) {
              /* Would dangle once the block leaves the list: #BKE_key_evaluate_object_ex silently
               * skips a block whose reference is out of range, so the dependent shape would go
               * quiet with no explanation. Remap to the basis and remember it for the redo. */
              op.relative_uids.append(other.uid);
              other.relative = 0;
            }
            else if (other.relative > index) {
              other.relative -= 1;
            }
          }
          BLI_remlink(&key.block, &kb);
          key.totkey--;
          op.bake_key = &kb;
          op.bake_key_index = index;
          op.pre_undo_shapenr = object.shapenr;
          if (object.shapenr > index) {
            object.shapenr = std::max(1, object.shapenr - 1);
          }
          break;
        }
      }
      else if (!is_undo && op.bake_key != nullptr) {
        KeyBlock *before = static_cast<KeyBlock *>(BLI_findlink(&key.block, op.bake_key_index));
        if (before) {
          BLI_insertlinkbefore(&key.block, before, op.bake_key);
        }
        else {
          BLI_addtail(&key.block, op.bake_key);
        }
        key.totkey++;
        /* Undo the index fix-up the detach did, in the reverse order: shift back the blocks it
         * decremented, then re-point the ones it remapped to the basis. The remapped blocks are
         * excluded from the shift explicitly rather than by relying on where the block landed, so
         * the two passes stay independent of its index. */
        const int index = BLI_findindex(&key.block, op.bake_key);
        for (KeyBlock &other : key.block) {
          if (&other == op.bake_key || op.relative_uids.contains(other.uid)) {
            continue;
          }
          if (other.relative >= index) {
            other.relative += 1;
          }
        }
        for (const int uid : op.relative_uids) {
          if (KeyBlock *other = BKE_keyblock_find_uid(&key, uid)) {
            other->relative = short(index);
          }
        }
        op.relative_uids.clear();
        object.shapenr = op.pre_undo_shapenr;
        op.bake_key = nullptr;
        op.bake_key_index = -1;
      }
    }

    /* A second pass, because the layer-list swap above reinserts nodes whose #flag comes straight
     * out of the payload (#node_metadata_from_payload copies the whole word), so a layer captured
     * while REC was armed carries its exemption back into a tree the first pass had already
     * settled. Re-derived here rather than stripped in the payload copy, so that a layer that
     * *should* be exempt — the active one, with REC still armed — gets the bit back rather than
     * merely losing it.
     *
     * Placed before everything below that composes or inverts: #BKE_object_shapekey_free (which in
     * this fork folds the layers into `vert_positions`) and #layers::session_state_ensure, whose
     * #bke::sculpt_layers::derive_base_mesh recovers the base at the weights in force when it runs.
     * The refresh inside #layers::commit_layers_change at the end of this branch is too late for
     * both: it would overwrite the bit those two had already been measured against, dropping the
     * difference into the base for good. Idempotent, so this costs one flag test per layer when the
     * pass above already settled the tree — the ordinary case. */
    layers::rec_exemption_refresh(object);

    /* Bake-to-shape-key undo: fully tear the #Key this step created back down. Runs after the
     * generic layer-list swap above has already reinserted the removed layers into the tree,
     * because #BKE_object_shapekey_free — in this fork — itself calls
     * #bke::sculpt_layers::bake_vert_layers_into_positions, which needs to see those layers to
     * fold their contribution back into `vert_positions`. */
    if (op.created_key && is_undo && mesh.key != nullptr) {
      Main *bmain_rw = CTX_data_main(C);
      BKE_object_shapekey_free(bmain_rw, &object);
      object.shapenr = op.pre_bake_shapenr;
    }

    if (op.is_bake && !op.removed.is_empty()) {
      /* A bake toggles between "contribution in the base" and "contribution in the layers" while
       * the combined surface stays identical. Re-derive the runtime mesh base against the
       * unchanged live positions and the new layer set so `base + layers` keeps matching them.
       * For plain add / remove changes the base derived above (before the list change) is the
       * correct one and must NOT be re-derived: the live positions do not yet reflect the new
       * layer set until the recompute below. */
      layers::invalidate_runtime(object);
      layers::session_state_ensure(object);
    }

    /* A structural undo (reparent / create / delete) can change a layer's ancestry and staleify its
     * cached folder-influence product, even though the influence values themselves are never undone
     * (folder influence is live, non-undoable slider state). Rebuild the float cache from the
     * restored tree before the recompute below reads it through #effective. Float-only, so it cannot
     * clobber the #SCULPT_LAYER_GROUP_HIDDEN / Solo-Base flags restored earlier in this branch. */
    bke::sculpt_layers::refresh_group_influence_cache(mesh);

    /* Bring the positions in sync with the restored layer state: canonical recompute for the
     * mesh domain, honest re-evaluation for multires. */
    layers::commit_layers_change(*depsgraph, object);
    return;
  }

  /* Taken here rather than at the top of the function: the mask session hook above can re-evaluate
   * the depsgraph and replace the tree (see the #bke::object::pbvh_ensure call there). Cheap when
   * the tree is already built, which is the case on every path that reaches this point. */
  bke::pbvh::Tree &pbvh = bke::object::pbvh_ensure(*depsgraph, object);

  /* Adding multires via the `subdivision_set` operator results in the subsequent undo step
   * not correctly performing a global undo step; we exit early here to avoid crashing.
   * See: #131478 */
  const bool multires_undo_step = use_multires_undo(step_data, ss);
  if ((multires_undo_step && pbvh.type() != bke::pbvh::Type::Grids) ||
      (!multires_undo_step && pbvh.type() != bke::pbvh::Type::Mesh))
  {
    CLOG_WARN(&LOG,
              "Undo step type and sculpt geometry type do not match: skipping undo state restore");
    return;
  }

  const bool tag_update = ID_REAL_USERS(object.data) > 1 ||
                          !BKE_sculptsession_use_pbvh_draw(&object, rv3d) || ss.shapekey_active ||
                          ss.deform_modifiers_active;

  switch (step_data.type) {
    case Type::None: {
      BLI_assert_unreachable();
      break;
    }
    case Type::Position: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object, pbvh)) {
        return;
      }

      if (use_multires_undo(step_data, ss)) {
        if (step_data.sculpt_layer_uid != 0 && step_data.sculpt_layer_grids) {
          /* A stroke recorded into a grid sculpt layer: the base MDisps were not modified by the
           * stroke, so the CCG positions are not restored directly. Revert the layer's explicit
           * delta and re-evaluate: the composed surface (base + layers) is a deterministic
           * function of the stored data. Skipping #multires_mark_as_modified is intentional —
           * a later flush must not bake the composed surface into the base.
           *
           * Consume any pending base edits first (e.g. left dirty by a previously restored
           * base-stroke step): the layer data is about to change, and a later flush against the
           * changed layer set would leak the delta into the base. */
          layers::flush_pending_multires_base(object);
          restore_active_sculpt_layer(step_data, object, is_undo);
          Mesh &mesh = *id_cast<Mesh *>(object.data);
          DEG_id_tag_update(&mesh.id, ID_RECALC_GEOMETRY);
          break;
        }
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

        Array<bool> modified_grids(subdiv_ccg.grids_num, false);
        restore_position_grids(
            subdiv_ccg.positions, key, *step_data.position_step_storage, modified_grids);

        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_grids, nodes[i].grids()); },
            exec_mode::grain_size(1));
        pbvh.tag_positions_changed(changed_nodes);
        multires_mark_as_modified(depsgraph, &object, MULTIRES_COORDS_MODIFIED);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        if (!restore_active_shape_key(*C, *depsgraph, step_data, object)) {
          return;
        }
        const Mesh &mesh = *id_cast<const Mesh *>(object.data);
        Array<bool> modified_verts(mesh.verts_num, false);

        /* Delegate the position restore to #restore_active_sculpt_layer only when the recorded
         * layer deltas are actually applicable; otherwise fall back to the plain swap so the
         * positions are still undone (the layer data is gone with the layer in that case). */
        const bool skip_positions_swap = can_restore_active_sculpt_layer_mesh(step_data, object);
        if (!skip_positions_swap && step_data.sculpt_layer_uid != 0 &&
            !step_data.sculpt_layer_verts.is_empty())
        {
          CLOG_WARN(&LOG,
                    "Recorded sculpt-layer deltas not applicable (layer removed out-of-band?); "
                    "falling back to plain position restore");
        }
        restore_position_mesh(
            object, *step_data.position_step_storage, modified_verts, skip_positions_swap);

        /* Keep the active sculpt layer in sync with the restored positions. This has to run before
         * the bounds are recomputed below: with `skip_positions_swap` set #restore_position_mesh
         * deliberately leaves the positions alone, so this call is what actually moves them.
         * Recomputing first would copy boxes describing the still-deformed surface into
         * #bounds_orig_ (and #store_bounds_orig clears the dirty accumulator, so nothing catches up
         * later), and the next stroke would drop nodes whose geometry moved back under the cursor
         * while deforming their neighbors — the mesh tears along leaf-node borders. */
        restore_active_sculpt_layer(step_data, object, is_undo);

        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              return indices_contain_true(modified_verts, nodes[i].all_verts());
            },
            exec_mode::grain_size(1));
        pbvh.tag_positions_changed(changed_nodes);
      }

      if (tag_update) {
        Mesh &mesh = *id_cast<Mesh *>(object.data);
        mesh.tag_positions_changed();
        BKE_sculptsession_free_deformMats(&ss);
      }
      else {
        Mesh &mesh = *id_cast<Mesh *>(object.data);
        /* The BVH normals recalculation that will happen later (caused by
         * `pbvh.tag_positions_changed`) won't recalculate the face corner normals.
         * We need to manually clear that cache. */
        mesh.runtime->corner_normals_cache.tag_dirty();
      }
      pbvh.update_bounds(*depsgraph, object);
      bke::pbvh::store_bounds_orig(pbvh);

      /* A base-editing stroke (not recorded into a layer) folds itself into the runtime layer base
       * at stroke end; undoing it restores the positions but leaves that cached base advanced by
       * the now-undone stroke. Invalidate it so the next stroke re-derives the base from the
       * restored positions instead of a phantom surface. Recorded-layer strokes (uid != 0) keep the
       * base unchanged and are already reconciled by #restore_active_sculpt_layer above. */
      if (step_data.sculpt_layer_uid == 0) {
        layers::invalidate_runtime(object);
      }
      break;
    }
    case Type::HideVert: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object, pbvh)) {
        return;
      }

      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        Array<bool> modified_grids(subdiv_ccg.grids_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_vert_visibility_grids(subdiv_ccg, *unode, modified_grids);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_grids, nodes[i].grids()); },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const Mesh &mesh = *id_cast<const Mesh *>(object.data);
        Array<bool> modified_verts(mesh.verts_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_vert_visibility_mesh(object, *unode, modified_verts);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              return indices_contain_true(modified_verts, nodes[i].all_verts());
            },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }

      BKE_pbvh_sync_visibility_from_verts(object);
      pbvh.update_visibility(object);
      if (BKE_sculpt_multires_active(scene, &object)) {
        multires_mark_as_modified(depsgraph, &object, MULTIRES_HIDDEN_MODIFIED);
      }
      break;
    }
    case Type::HideFace: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object, pbvh)) {
        return;
      }

      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      Array<bool> modified_faces(mesh.faces_num, false);
      for (std::unique_ptr<Node> &unode : step_data.nodes) {
        restore_hidden_face(object, *unode, modified_faces);
      }

      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              Vector<int> faces_vector;
              const Span<int> faces = bke::pbvh::node_face_indices_calc_grids(
                  subdiv_ccg, nodes[i], faces_vector);
              return indices_contain_true(modified_faces, faces);
            },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_faces, nodes[i].faces()); },
            exec_mode::grain_size(1));
        pbvh.tag_visibility_changed(changed_nodes);
      }

      hide::sync_all_from_faces(object);
      pbvh.update_visibility(object);
      break;
    }
    case Type::Mask: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object, pbvh)) {
        return;
      }

      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        Array<bool> modified_grids(ss.subdiv_ccg->grids_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_mask_grids(object, *unode, modified_grids);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_grids, nodes[i].grids()); },
            exec_mode::grain_size(1));
        bke::pbvh::update_mask_grids(*ss.subdiv_ccg, changed_nodes, pbvh);
        pbvh.tag_masks_changed(changed_nodes);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const Mesh &mesh = *id_cast<const Mesh *>(object.data);
        Array<bool> modified_verts(mesh.verts_num, false);
        for (std::unique_ptr<Node> &unode : step_data.nodes) {
          restore_mask_mesh(object, *unode, modified_verts);
        }
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              return indices_contain_true(modified_verts, nodes[i].all_verts());
            },
            exec_mode::grain_size(1));
        bke::pbvh::update_mask_mesh(mesh, changed_nodes, pbvh);
        pbvh.tag_masks_changed(changed_nodes);
      }
      break;
    }
    case Type::FaceSet: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object, pbvh)) {
        return;
      }

      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      Array<bool> modified_faces(mesh.faces_num, false);
      for (std::unique_ptr<Node> &unode : step_data.nodes) {
        restore_face_sets(object, *unode, modified_faces);
      }
      if (use_multires_undo(step_data, ss)) {
        MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
        const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) {
              Vector<int> faces_vector;
              const Span<int> faces = bke::pbvh::node_face_indices_calc_grids(
                  subdiv_ccg, nodes[i], faces_vector);
              return indices_contain_true(modified_faces, faces);
            },
            exec_mode::grain_size(1));
        pbvh.tag_face_sets_changed(changed_nodes);
      }
      else {
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        const IndexMask changed_nodes = IndexMask::from_predicate(
            node_mask,
            memory,
            [&](const int i) { return indices_contain_true(modified_faces, nodes[i].faces()); },
            exec_mode::grain_size(1));
        pbvh.tag_face_sets_changed(changed_nodes);
      }
      break;
    }
    case Type::Color: {
      IndexMaskMemory memory;
      const IndexMask node_mask = bke::pbvh::all_leaf_nodes(pbvh, memory);

      BKE_sculpt_update_object_for_edit(depsgraph, &object, false);
      if (!topology_matches(step_data, object, pbvh)) {
        return;
      }

      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      const Mesh &mesh = *id_cast<const Mesh *>(object.data);
      Array<bool> modified_verts(mesh.verts_num, false);
      restore_color(object, step_data, modified_verts);
      const IndexMask changed_nodes = IndexMask::from_predicate(
          node_mask,
          memory,
          [&](const int i) { return indices_contain_true(modified_verts, nodes[i].all_verts()); },
          exec_mode::grain_size(1));
      pbvh.tag_attribute_changed(changed_nodes, mesh.active_color_attribute);
      break;
    }
    case Type::Geometry: {
      BLI_assert(!ss.bm);

      restore_geometry(step_data, object);
      BKE_sculptsession_free_deformMats(&ss);
      if (SubdivCCG *subdiv_ccg = ss.subdiv_ccg) {
        refine_subdiv(depsgraph, ss, object, subdiv_ccg->subdiv);
      }
      break;
    }
    case Type::DyntopoBegin:
    case Type::DyntopoEnd:
      /* Handled elsewhere. */
      BLI_assert_unreachable();
      break;
    case Type::SculptLayer:
      /* Handled via early return above (before the pbvh-type check). */
      BLI_assert_unreachable();
      break;
  }

  DEG_id_tag_update(&object.id, ID_RECALC_SHADING);
  if (tag_update) {
    DEG_id_tag_update(&object.id, ID_RECALC_GEOMETRY);
  }
}

/* \a is_undo is the decode direction, threaded straight through to each object's
 * #restore_list_object; see the note there for why it is a parameter rather than
 * #StepData::needs_undo. */
static void restore_list(bContext *C,
                         Depsgraph *depsgraph,
                         Vector<std::unique_ptr<StepData>> &objects_data,
                         const bool is_undo)
{
  for (std::unique_ptr<StepData> &sd_ptr : objects_data) {
    StepData &sd = *sd_ptr;
    /* Resolved to a live pointer by #step_resolve_id_refs at the top of #step_decode; null if the
     * object was since deleted or could not be found in the current #Main. */
    Object *ob = sd.object_ref.ptr;
    if (!ob) {
      continue;
    }

    if (sd.needs_undo()) {
      restore_list_object(C, depsgraph, sd, *ob, is_undo);
      sd.tag_needs_redo();
    }
    else {
      restore_list_object(C, depsgraph, sd, *ob, is_undo);
      sd.tag_needs_undo();
    }

    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
}

static void free_step_data_resources(StepData &step_data)
{
  if (KeyBlock *kb = step_data.sculpt_layer_op.bake_key) {
    /* The step still owns the key block a bake created and an undo detached from the mesh (the redo
     * that would link it back can no longer happen once the step is freed). Mirrors the tail of
     * #BKE_object_shapekey_remove; the `relative` fix-up that call also does already happened at
     * detach time (see #restore_list).
     *
     * TODO: drivers keyed to this block's RNA path survive on the #Key, because
     * #BKE_animdata_drivers_remove_for_rna_struct needs a #Main that is not reachable here. The
     * detach itself must not remove them (a redo needs them back), so only this path leaks them. */
    if (kb->data) {
      MEM_delete_void(kb->data);
    }
    MEM_delete(kb);
    step_data.sculpt_layer_op.bake_key = nullptr;
  }
  geometry_free_data(&step_data.geometry_original);
  geometry_free_data(&step_data.geometry_modified);
  geometry_free_data(&step_data.bmesh.geometry_enter);
  if (step_data.bmesh.bm_entry) {
    BM_log_entry_drop(step_data.bmesh.bm_entry);
    step_data.bmesh.bm_entry = nullptr;
  }
}

/**
 * Retrieve the undo data of a given type for the active undo step. For example, this is used to
 * access "original" data from before the current stroke.
 *
 * This is only possible when building an undo step, in between #push_begin and #push_end.
 */
static const Node *get_node(const Object &ob, const bke::pbvh::Node *node, const Type type)
{
  StepData *step_data = get_step_data(ob);
  if (!step_data) {
    return nullptr;
  }
  if (step_data->type != type) {
    return nullptr;
  }
  /* This access does not need to be locked because this function is not expected to be called
   * while the per-node undo data is being pushed. In other words, this must not be called
   * concurrently with #push_node. */
  std::unique_ptr<Node> *node_ptr = step_data->undo_nodes_by_pbvh_node.lookup_ptr(node);
  if (!node_ptr) {
    return nullptr;
  }
  return node_ptr->get();
}

static void store_vert_visibility_grids(const SubdivCCG &subdiv_ccg,
                                        const bke::pbvh::GridsNode &node,
                                        Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  if (grid_hidden.is_empty()) {
    return;
  }

  const Span<int> grid_indices = node.grids();
  unode.grid_hidden = BitGroupVector<0>(grid_indices.size(), grid_hidden.group_size());
  for (const int i : grid_indices.index_range()) {
    unode.grid_hidden[i].copy_from(grid_hidden[grid_indices[i]]);
  }
}

static void store_positions_mesh(const Depsgraph &depsgraph, const Object &object, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  gather_data_mesh(bke::pbvh::vert_positions_eval(depsgraph, object),
                   unode.vert_indices.as_span(),
                   unode.position.as_mutable_span());
  gather_data_mesh(bke::pbvh::vert_normals_eval(depsgraph, object),
                   unode.vert_indices.as_span(),
                   unode.normal.as_mutable_span());

  if (ss.deform_modifiers_active) {
    const Mesh &mesh = *id_cast<const Mesh *>(object.data);
    const Span<float3> orig_positions = ss.shapekey_active ? Span(static_cast<const float3 *>(
                                                                      ss.shapekey_active->data),
                                                                  mesh.verts_num) :
                                                             mesh.vert_positions();

    gather_data_mesh(
        orig_positions, unode.vert_indices.as_span(), unode.orig_position.as_mutable_span());
  }
}

static void store_positions_grids(const SubdivCCG &subdiv_ccg, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  gather_data_grids(
      subdiv_ccg, subdiv_ccg.positions.as_span(), unode.grids, unode.position.as_mutable_span());
  gather_data_grids(
      subdiv_ccg, subdiv_ccg.normals.as_span(), unode.grids, unode.normal.as_mutable_span());
}

static void store_vert_visibility_mesh(const Mesh &mesh, const bke::pbvh::Node &node, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_vert = *attributes.lookup<bool>(".hide_vert",
                                                              bke::AttrDomain::Point);
  if (hide_vert.is_empty()) {
    return;
  }

  const Span<int> verts = static_cast<const bke::pbvh::MeshNode &>(node).all_verts();
  for (const int i : verts.index_range()) {
    unode.vert_hidden[i].set(hide_vert[verts[i]]);
  }
}

static void store_face_visibility(const Mesh &mesh, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<bool> hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  if (hide_poly.is_empty()) {
    unode.face_hidden.fill(false);
    return;
  }
  const Span<int> faces = unode.face_indices;
  for (const int i : faces.index_range()) {
    unode.face_hidden[i].set(hide_poly[faces[i]]);
  }
}

static void store_mask_mesh(const Mesh &mesh, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  if (mask.is_empty()) {
    unode.mask.fill(0.0f);
  }
  else {
    gather_data_mesh(mask, unode.vert_indices.as_span(), unode.mask.as_mutable_span());
  }
}

static void store_mask_grids(const SubdivCCG &subdiv_ccg, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  if (!subdiv_ccg.masks.is_empty()) {
    gather_data_grids(
        subdiv_ccg, subdiv_ccg.masks.as_span(), unode.grids, unode.mask.as_mutable_span());
  }
  else {
    unode.mask.fill(0.0f);
  }
}

static void store_color(const Mesh &mesh, const bke::pbvh::MeshNode &node, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = mesh.vert_to_face_map();
  const bke::GAttributeReader color_attribute = color::active_color_attribute(mesh);
  const GVArraySpan colors(*color_attribute);

  /* NOTE: even with loop colors we still store (derived)
   * vertex colors for original data lookup. */
  const Span<int> verts = node.verts();
  unode.col.reinitialize(verts.size());
  color::gather_colors_vert(
      faces, corner_verts, vert_to_face_map, colors, color_attribute.domain, verts, unode.col);

  if (color_attribute.domain == bke::AttrDomain::Corner) {
    for (const int face : node.faces()) {
      for (const int corner : faces[face]) {
        unode.corner_indices.append(corner);
      }
    }
    unode.loop_col.reinitialize(unode.corner_indices.size());
    color::gather_colors(colors, unode.corner_indices, unode.loop_col);
  }
}

static NodeGeometry *geometry_get(StepData &step_data)
{
  if (!step_data.geometry_original.is_initialized) {
    return &step_data.geometry_original;
  }

  BLI_assert(!step_data.geometry_modified.is_initialized);

  return &step_data.geometry_modified;
}

static void geometry_push(const Object &object)
{
  StepData *step_data = get_step_data(object);

  step_data->type = Type::Geometry;

  NodeGeometry *geometry = geometry_get(*step_data);
  store_geometry_data(geometry, object);
}

static void store_face_sets(const Mesh &mesh, Node &unode)
{
  PRF_scope(ProfileCategory::Editor);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan face_sets = *attributes.lookup<int>(".sculpt_face_set", bke::AttrDomain::Face);
  if (face_sets.is_empty()) {
    unode.face_sets.fill(1);
  }
  else {
    gather_data_mesh(face_sets, unode.face_indices.as_span(), unode.face_sets.as_mutable_span());
  }
}

static void fill_node_data_mesh(const Depsgraph &depsgraph,
                                const Object &object,
                                const bke::pbvh::MeshNode &node,
                                const Type type,
                                Node &unode)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Mesh &mesh = *id_cast<Mesh *>(object.data);

  unode.vert_indices = node.all_verts();
  unode.unique_verts_num = node.verts().size();

  const int verts_num = unode.vert_indices.size();

  if (ELEM(type, Type::FaceSet, Type::HideFace)) {
    unode.face_indices = node.faces();
  }

  switch (type) {
    case Type::None:
      BLI_assert_unreachable();
      break;
    case Type::Position: {
      unode.position.reinitialize(verts_num);
      /* Needed for original data lookup. */
      unode.normal.reinitialize(verts_num);
      if (ss.deform_modifiers_active) {
        unode.orig_position.reinitialize(verts_num);
      }
      store_positions_mesh(depsgraph, object, unode);
      break;
    }
    case Type::HideVert: {
      unode.vert_hidden.resize(unode.vert_indices.size());
      store_vert_visibility_mesh(mesh, node, unode);
      break;
    }
    case Type::HideFace: {
      unode.face_hidden.resize(unode.face_indices.size());
      store_face_visibility(mesh, unode);
      break;
    }
    case Type::Mask: {
      unode.mask.reinitialize(verts_num);
      store_mask_mesh(mesh, unode);
      break;
    }
    case Type::Color: {
      store_color(mesh, node, unode);
      break;
    }
    case Type::DyntopoBegin:
    case Type::DyntopoEnd:
      /* Dyntopo should be handled elsewhere. */
      BLI_assert_unreachable();
      break;
    case Type::Geometry:
      /* See #geometry_push. */
      BLI_assert_unreachable();
      break;
    case Type::FaceSet: {
      unode.face_sets.reinitialize(unode.face_indices.size());
      store_face_sets(mesh, unode);
      break;
    }
    case Type::SculptLayer:
      /* Operator-level layer undo captures metadata/data in StepData directly, not per-node. */
      break;
  }
}

static void fill_node_data_grids(const Object &object,
                                 const bke::pbvh::GridsNode &node,
                                 const Type type,
                                 Node &unode)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Mesh &base_mesh = *id_cast<const Mesh *>(object.data);
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  unode.grids = node.grids();

  const int grid_area = subdiv_ccg.grid_size * subdiv_ccg.grid_size;
  const int verts_num = unode.grids.size() * grid_area;

  if (ELEM(type, Type::FaceSet, Type::HideFace)) {
    bke::pbvh::node_face_indices_calc_grids(subdiv_ccg, node, unode.face_indices);
  }

  switch (type) {
    case Type::None:
      BLI_assert_unreachable();
      break;
    case Type::Position: {
      unode.position.reinitialize(verts_num);
      /* Needed for original data lookup. */
      unode.normal.reinitialize(verts_num);
      store_positions_grids(subdiv_ccg, unode);
      break;
    }
    case Type::HideVert: {
      store_vert_visibility_grids(subdiv_ccg, node, unode);
      break;
    }
    case Type::HideFace: {
      unode.face_hidden.resize(unode.face_indices.size());
      store_face_visibility(base_mesh, unode);
      break;
    }
    case Type::Mask: {
      unode.mask.reinitialize(verts_num);
      store_mask_grids(subdiv_ccg, unode);
      break;
    }
    case Type::Color: {
      BLI_assert_unreachable();
      break;
    }
    case Type::DyntopoBegin:
    case Type::DyntopoEnd:
      /* Dyntopo should be handled elsewhere. */
      BLI_assert_unreachable();
      break;
    case Type::Geometry:
      /* See #geometry_push. */
      BLI_assert_unreachable();
      break;
    case Type::FaceSet: {
      unode.face_sets.reinitialize(unode.face_indices.size());
      store_face_sets(base_mesh, unode);
      break;
    }
    case Type::SculptLayer:
      /* Operator-level layer undo captures metadata/data in StepData directly, not per-node. */
      break;
  }
}

/**
 * Dynamic topology stores only one undo node per stroke, regardless of the number of
 * bke::pbvh::Tree nodes modified.
 */
BLI_NOINLINE static void bmesh_push(const Object &object,
                                    const bke::pbvh::BMeshNode *node,
                                    Type type)
{
  StepData *step_data = get_step_data(object);
  const SculptSession &ss = *object.runtime->sculpt_session;

  std::scoped_lock lock(*step_data->nodes_mutex);

  if (step_data->nodes.is_empty()) {
    /* We currently need to append data here so that the overall undo system knows to indicate that
     * data should be flushed to the memfile */
    /* TODO: Once we store entering Sculpt Mode as a specific type of action, we can remove this
     * call. */
    step_data->nodes.append(std::make_unique<Node>());

    step_data->type = type;

    if (type == Type::DyntopoEnd) {
      step_data->bmesh.bm_entry = BM_log_entry_add(ss.bm_log);
      BM_log_before_all_removed(ss.bm, ss.bm_log);
    }
    else if (type == Type::DyntopoBegin) {
      /* Store a copy of the mesh's current vertices, loops, and
       * faces. A full copy like this is needed because entering
       * dynamic-topology immediately does topological edits
       * (converting faces to triangles) that the BMLog can't
       * fully restore from. */
      NodeGeometry *geometry = &step_data->bmesh.geometry_enter;
      store_geometry_data(geometry, object);

      step_data->bmesh.bm_entry = BM_log_entry_add(ss.bm_log);
      BM_log_all_added(ss.bm, ss.bm_log);
    }
    else {
      step_data->bmesh.bm_entry = BM_log_entry_add(ss.bm_log);
    }
  }

  if (node) {
    const int cd_vert_mask_offset = CustomData_get_offset_named(
        &ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");

    /* The vertices and node aren't changed, though pointers to them are stored in the log. */
    bke::pbvh::BMeshNode *node_mut = const_cast<bke::pbvh::BMeshNode *>(node);

    switch (type) {
      case Type::None:
        BLI_assert_unreachable();
        break;
      case Type::Position:
      case Type::Mask:
        /* Before any vertex values get modified, ensure their
         * original positions are logged. */
        for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }
        for (BMVert *vert : BKE_pbvh_bmesh_node_other_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }
        break;

      case Type::HideFace:
      case Type::HideVert: {
        for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }
        for (BMVert *vert : BKE_pbvh_bmesh_node_other_verts(node_mut)) {
          BM_log_vert_before_modified(ss.bm_log, vert, cd_vert_mask_offset);
        }

        for (BMFace *f : BKE_pbvh_bmesh_node_faces(node_mut)) {
          BM_log_face_modified(ss.bm_log, f);
        }
        break;
      }

      case Type::DyntopoBegin:
      case Type::DyntopoEnd:
      case Type::Geometry:
      case Type::FaceSet:
      case Type::Color:
      case Type::SculptLayer:
        break;
    }
  }
}

/**
 * Add an undo node for the bke::pbvh::Tree node to the step's storage. If the node was
 * newly created and needs to be filled with data, set \a r_new to true.
 */
static Node *ensure_node(StepData &step_data, const bke::pbvh::Node &node, bool &r_new)
{
  std::scoped_lock lock(*step_data.nodes_mutex);
  r_new = false;
  std::unique_ptr<Node> &unode = step_data.undo_nodes_by_pbvh_node.lookup_or_add_cb(&node, [&]() {
    std::unique_ptr<Node> new_unode = std::make_unique<Node>();
    r_new = true;
    return new_unode;
  });
  return unode.get();
}

void push_node(const Depsgraph &depsgraph,
               const Object &object,
               const bke::pbvh::Node *node,
               const Type type)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (ss.bm || ELEM(type, Type::DyntopoBegin, Type::DyntopoEnd)) {
    bmesh_push(object, static_cast<const bke::pbvh::BMeshNode *>(node), type);
    return;
  }

  StepData *step_data = get_step_data(object);

  /* #push_node is called concurrently from worker threads for different nodes of the same object
   * (see the parallel `foreach_index` callers in paint_mask/paint_hide/sculpt_face_set). The step
   * type and topology counts are written once per object but reached from every node, so serialize
   * the (idempotent) writes under the step's mutex to avoid a data race. The lock is released
   * before #ensure_node, which acquires the same mutex itself. */
  {
    std::scoped_lock lock(*step_data->nodes_mutex);
    BLI_assert(ELEM(step_data->type, Type::None, type));
    step_data->type = type;
    ensure_step_topology_data(const_cast<Object &>(object), *step_data);
  }

  bool newly_added;
  Node *unode = ensure_node(*step_data, *node, newly_added);
  if (!newly_added) {
    /* The node was already filled with data for this undo step. */
    return;
  }

  ss.needs_flush_to_id = true;
  ss.pbvh_draw_required = true;

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      fill_node_data_mesh(
          depsgraph, object, static_cast<const bke::pbvh::MeshNode &>(*node), type, *unode);
      break;
    case bke::pbvh::Type::Grids:
      fill_node_data_grids(object, static_cast<const bke::pbvh::GridsNode &>(*node), type, *unode);
      break;
    case bke::pbvh::Type::BMesh:
      BLI_assert_unreachable();
      break;
  }
}

void push_nodes(const Depsgraph &depsgraph,
                Object &object,
                const IndexMask &node_mask,
                const Type type)
{
  PRF_scope(ProfileCategory::Editor);
  SculptSession &ss = *object.runtime->sculpt_session;

  ss.needs_flush_to_id = true;
  ss.pbvh_draw_required = true;

  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (ss.bm || ELEM(type, Type::DyntopoBegin, Type::DyntopoEnd)) {
    const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
    node_mask.foreach_index([&](const int i) { bmesh_push(object, &nodes[i], type); });
    return;
  }

  StepData *step_data = get_step_data(object);
  BLI_assert(ELEM(step_data->type, Type::None, type));
  step_data->type = type;
  ensure_step_topology_data(object, *step_data);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      Vector<std::pair<const bke::pbvh::MeshNode *, Node *>, 32> nodes_to_fill;
      node_mask.foreach_index([&](const int i) {
        bool newly_added;
        Node *unode = ensure_node(*step_data, nodes[i], newly_added);
        if (newly_added) {
          nodes_to_fill.append({&nodes[i], unode});
        }
      });
      threading::parallel_for(nodes_to_fill.index_range(), 1, [&](const IndexRange range) {
        for (const auto &[node, unode] : nodes_to_fill.as_span().slice(range)) {
          fill_node_data_mesh(depsgraph, object, *node, type, *unode);
        }
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      Vector<std::pair<const bke::pbvh::GridsNode *, Node *>, 32> nodes_to_fill;
      node_mask.foreach_index([&](const int i) {
        bool newly_added;
        Node *unode = ensure_node(*step_data, nodes[i], newly_added);
        if (newly_added) {
          nodes_to_fill.append({&nodes[i], unode});
        }
      });
      threading::parallel_for(nodes_to_fill.index_range(), 1, [&](const IndexRange range) {
        for (const auto &[node, unode] : nodes_to_fill.as_span().slice(range)) {
          fill_node_data_grids(object, *node, type, *unode);
        }
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      BLI_assert_unreachable();
      break;
    }
  }
}

static void save_active_attribute(Object &object, SculptAttrRef *attr)
{
  Mesh *mesh = BKE_object_get_original_mesh(&object);
  attr->was_set = true;
  attr->domain = NO_ACTIVE_LAYER;
  attr->name[0] = 0;
  if (!mesh) {
    return;
  }
  const char *name = mesh->active_color_attribute;
  const bke::AttributeAccessor attributes = mesh->attributes();
  const std::optional<bke::AttributeMetaData> meta_data = attributes.lookup_meta_data(name);
  if (!bke::mesh::is_color_attribute(meta_data)) {
    return;
  }
  attr->domain = meta_data->domain;
  STRNCPY_UTF8(attr->name, name);
  attr->type = *bke::attr_type_to_custom_data_type(meta_data->data_type);
}

/**
 * Does not save topology counts, as that data is unneeded for full geometry pushes and
 * requires the PBVH to exist.
 */
static void save_common_data(Object &ob, SculptUndoStep *us)
{
  StepData *step_data = get_step_data(ob);

  if (!us->active_color_start.was_set) {
    save_active_attribute(ob, &us->active_color_start);
  }

  /* Set end attribute in case push_end is not called,
   * so we don't end up with corrupted state.
   */
  if (!us->active_color_end.was_set) {
    save_active_attribute(ob, &us->active_color_end);
    us->active_color_end.was_set = false;
  }

  if (ob.runtime->sculpt_session == nullptr) {
    return;
  }

  const SculptSession &ss = *ob.runtime->sculpt_session;

  step_data->pivot_pos = ss.pivot_pos;
  step_data->pivot_rot = ss.pivot_rot;

  if (const KeyBlock *key = BKE_keyblock_from_object(&ob)) {
    step_data->active_shape_key_name = key->name;
  }
}

/**
 * Store topology counts used by #topology_matches during undo/redo.
 * Always reads mesh grid counts from the object data so secondary sculpt objects
 * without a built PBVH at stroke start still restore correctly.
 */
static void save_step_topology_data(Object &ob, SculptUndoStep *us)
{
  save_common_data(ob, us);
  StepData *step_data = get_step_data(ob);
  if (!step_data) {
    return;
  }

  if (ob.runtime->sculpt_session == nullptr) {
    save_mesh_topology_data(ob, *step_data);
    return;
  }

  const SculptSession &ss = *ob.runtime->sculpt_session;
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);

  if (pbvh) {
    switch (pbvh->type()) {
      case bke::pbvh::Type::Mesh: {
        save_mesh_topology_data(ob, *step_data);
        break;
      }
      case bke::pbvh::Type::Grids: {
        save_grids_topology_data(ss, *step_data);
        break;
      }
      case bke::pbvh::Type::BMesh: {
        break;
      }
    }
  }
  else {
    /* PBVH may not exist yet for objects that were not under the cursor when the stroke began.
     * Fall back to mesh topology so undo restore is not rejected by #topology_matches. */
    save_mesh_topology_data(ob, *step_data);
    save_grids_topology_data(ss, *step_data);
  }
}

void push_begin_ex(const Scene & /*scene*/, Object &ob, const char *name)
{
  UndoStack *ustack = ED_undo_stack_get();

  /* If possible, we need to tag the object and its geometry data as 'changed in the future' in
   * the previous undo step if it's a memfile one. */
  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  /* Special case, we never read from this. */
  bContext *C = nullptr;

  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_step_push_init_with_type(ustack, C, name, BKE_UNDOSYS_TYPE_SCULPT));

  save_step_topology_data(ob, us);
}

void push_begin_add_object(Object &ob)
{
  UndoStack *ustack = ED_undo_stack_get();

  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  SculptUndoStep *us = get_init_sculpt_step();
  if (!us) {
    return;
  }
  save_step_topology_data(ob, us);
}

void set_object_transform_snapshot(Object &ob)
{
  StepData *step_data = get_step_data(ob);
  if (!step_data) {
    return;
  }
  step_data->object_transform = ob.object_to_world();
  step_data->has_object_transform = true;
}

void restore_object_transform_from_undo_step(Object &ob)
{
  StepData *step_data = get_step_data(ob);
  if (!step_data || !step_data->has_object_transform) {
    return;
  }
  BKE_object_apply_mat4(&ob, step_data->object_transform.ptr(), false, true);
  DEG_id_tag_update(&ob.id, ID_RECALC_TRANSFORM);
}

bool get_object_transform_snapshot(const Object &ob, float4x4 &r_transform)
{
  StepData *step_data = get_step_data(ob);
  if (!step_data || !step_data->has_object_transform) {
    return false;
  }
  r_transform = step_data->object_transform;
  return true;
}

void push_begin(const Scene &scene, Object &ob, const wmOperator *op)
{
  push_begin_ex(scene, ob, op->type->name);
}

void push_enter_sculpt_mode(const Scene & /*scene*/, Object &ob, const wmOperator *op)
{
  UndoStack *ustack = ED_undo_stack_get();

  /* If possible, we need to tag the object and its geometry data as 'changed in the future' in
   * the previous undo step if it's a memfile one. */
  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  /* Special case, we never read from this. */
  bContext *C = nullptr;

  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_step_push_init_with_type(ustack, C, op->type->name, BKE_UNDOSYS_TYPE_SCULPT));
  save_common_data(ob, us);
}

void push_enter_sculpt_mode_add_object(Object &ob)
{
  UndoStack *ustack = ED_undo_stack_get();

  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  SculptUndoStep *us = get_init_sculpt_step();
  if (!us) {
    return;
  }
  save_common_data(ob, us);
}

static size_t node_size_in_bytes(const Node &node)
{
  size_t size = sizeof(Node);
  size += node.position.as_span().size_in_bytes();
  size += node.orig_position.as_span().size_in_bytes();
  size += node.normal.as_span().size_in_bytes();
  size += node.col.as_span().size_in_bytes();
  size += node.mask.as_span().size_in_bytes();
  size += node.loop_col.as_span().size_in_bytes();
  size += node.vert_indices.as_span().size_in_bytes();
  size += node.corner_indices.as_span().size_in_bytes();
  size += node.vert_hidden.size() / 8;
  size += node.face_hidden.size() / 8;
  size += node.grids.as_span().size_in_bytes();
  size += node.grid_hidden.all_bits().size() / 8;
  size += node.face_sets.as_span().size_in_bytes();
  size += node.face_indices.as_span().size_in_bytes();
  return size;
}

static void finalize_step_data(StepData &step_data)
{
  if (step_data.position_step_storage) {
    return;
  }

  if (!step_data.undo_nodes_by_pbvh_node.is_empty()) {
    /* Move undo node storage from map to vector. */
    step_data.nodes.reserve(step_data.undo_nodes_by_pbvh_node.size());
    for (std::unique_ptr<Node> &node : step_data.undo_nodes_by_pbvh_node.values()) {
      step_data.nodes.append(std::move(node));
    }
    step_data.undo_nodes_by_pbvh_node.clear();
  }

  if (step_data.nodes.is_empty() && step_data.type == Type::None) {
    return;
  }

  /* We don't need normals in the undo stack. */
  for (std::unique_ptr<Node> &unode : step_data.nodes) {
    unode->normal = {};
  }

  if (step_data.type == Type::Position) {
    step_data.position_step_storage = std::make_unique<PositionUndoStorage>(step_data);
    step_data.position_step_storage->ensure_compression_complete();
  }
  else if (step_data.undo_size == 0) {
    step_data.undo_size = threading::parallel_reduce(
        step_data.nodes.index_range(),
        16,
        0,
        [&](const IndexRange range, size_t size) {
          for (const int i : range) {
            size += node_size_in_bytes(*step_data.nodes[i]);
          }
          return size;
        },
        std::plus<size_t>());
  }
}

static void push_sculpt_undo_step_to_stack()
{
  UndoStack *ustack = ED_undo_stack_get();
  if (!ustack->step_init || ustack->step_init->type != BKE_UNDOSYS_TYPE_SCULPT) {
    return;
  }

  BKE_undosys_step_push_with_type(ustack, nullptr, nullptr, BKE_UNDOSYS_TYPE_SCULPT);

  wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
  if (wm->op_undo_depth == 0) {
    BKE_undosys_stack_limit_steps_and_memory_defaults(ustack);
  }
  WM_file_tag_modified();
}

void push_end_ex(Object &ob, const bool /*use_nested_undo*/, const bool finalize_undo_step)
{
  SculptUndoStep *us = get_init_sculpt_step();
  if (!us) {
    return;
  }

  if (StepData *step_data = get_step_data(ob)) {
    if (step_data_has_undo_content(*step_data)) {
      finalize_step_data(*step_data);
    }
    save_active_attribute(ob, &us->active_color_end);
  }

  if (!finalize_undo_step) {
    return;
  }

  if (!sculpt_step_should_push(*us)) {
    discard_init_sculpt_step();
    return;
  }

  push_sculpt_undo_step_to_stack();
}

void push_end_all_ex(const bool /*use_nested_undo*/, const bool finalize_undo_step)
{
  SculptUndoStep *us = get_init_sculpt_step();
  if (!us) {
    return;
  }

  for (std::unique_ptr<StepData> &step_data : us->objects_data) {
    if (step_data_has_undo_content(*step_data)) {
      finalize_step_data(*step_data);
    }
  }

  if (!finalize_undo_step) {
    return;
  }

  if (!sculpt_step_should_push(*us)) {
    discard_init_sculpt_step();
    return;
  }

  if (!us->objects_data.is_empty()) {
    /* #active_color_start is captured from the first object (the active object, stored first by
     * #stroke_undo_begin). Capture the end state from the same object so the active color
     * attribute is restored consistently on undo/redo. Must run BEFORE
     * #push_sculpt_undo_step_to_stack below: that call finalizes the step via
     * #BKE_undosys_step_push_with_type, which runs #step_encode -> #step_store_id_refs and clears
     * every #StepData.object_ref.ptr to null (see #StepData.object_ref's doc-comment) -- reading
     * it after the push would always see nullptr. */
    Object *active_ob = us->objects_data.first()->object_ref.ptr;
    if (active_ob) {
      save_active_attribute(*active_ob, &us->active_color_end);
    }
  }

  push_sculpt_undo_step_to_stack();
}

void discard_init_step()
{
  discard_init_sculpt_step();
}

void push_end(Object &ob)
{
  push_end_ex(ob, false, true);
}

void push_begin_multi_object(const Scene &scene,
                             const wmOperator *op,
                             Span<Object *> scene_objects)
{
  const int n = scene_objects.size();
  if (n == 0) {
    return;
  }
  push_begin(scene, *scene_objects[0], op);
  for (int i = 1; i < n; ++i) {
    push_begin_add_object(*scene_objects[i]);
  }
}

void finish_multi_object(bContext *C, Span<Object *> scene_objects, UpdateType update_type)
{
  /* Closing pending `push_begin_ex` (image paint, etc.) -- the multi-object undo step is owned
   * by the sculpt undo system, but the surrounding brush operator may have started an image
   * undo step in parallel; the image undo system already completes it itself when
   * #push_end_all_ex runs (it consults the sculpt step data to skip objects it did not touch). */
  push_end_all_ex(false, true);

  for (Object *ob : scene_objects) {
    flush_update_done(C, *ob, update_type);
    WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  }
}

/* -------------------------------------------------------------------- */
/** \name Implements ED Undo System
 * \{ */

static void set_active_layer(bContext *C, const SculptAttrRef *attr_ref)
{
  if (attr_ref->domain == bke::AttrDomain::Auto) {
    return;
  }

  Object *ob = CTX_data_active_object(C);
  Mesh *mesh = BKE_object_get_original_mesh(ob);

  SculptAttrRef existing;
  save_active_attribute(*ob, &existing);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();

  /* Temporary fix for #97408. This is a fundamental
   * bug in the undo stack; the operator code needs to push
   * an extra undo step before running an operator if a
   * non-memfile undo system is active.
   *
   * For now, detect if the layer does exist but with a different
   * domain and just unconvert it.
   */
  if (const bke::GAttributeReader attr = attributes.lookup(attr_ref->name)) {
    if (attr.domain != attr_ref->domain ||
        bke::cpp_type_to_custom_data_type(attr.varray.type()) != attr_ref->type)
    {
      AttributeOwner owner = AttributeOwner::from_id(&mesh->id);
      if (ed::geometry::convert_attribute(owner,
                                          mesh->attributes_for_write(),
                                          attr_ref->name,
                                          attr_ref->domain,
                                          *bke::custom_data_type_to_attr_type(attr_ref->type),
                                          nullptr))
      {
      }
    }
  }

  if (!attributes.contains(attr_ref->name)) {
    /* Memfile undo killed the layer; re-create it. */
    mesh->attributes_for_write().add(attr_ref->name,
                                     attr_ref->domain,
                                     *bke::custom_data_type_to_attr_type(attr_ref->type),
                                     bke::AttributeInitDefaultValue());
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }

  if (attributes.contains(attr_ref->name)) {
    BKE_id_attributes_active_color_set(&mesh->id, attr_ref->name);
  }
}

static void step_encode_init(bContext * /*C*/, UndoStep *us_p)
{
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);
  new (&us->objects_data) Vector<std::unique_ptr<StepData>>();
}

/**
 * Serialize every #StepData.object_ref from a live #Object pointer to a name + library path, and
 * clear the pointer. Mirrors the generic undo system's `undosys_id_ref_store`, but is driven from
 * #step_encode / #step_decode instead of being registered as #UndoType.step_foreach_ID_ref.
 *
 * Registering that hook is what the generic system uses to decide a step references IDs, which
 * also opts the step into memfile ordering (`WITH_GLOBAL_UNDO_CORRECT_ORDER`, `undo_system.cc`):
 * decoding such a step force-loads the nearest preceding memfile step whenever that is not the
 * active memfile, re-reading the whole #Main. Operators like #SCULPT_OT_mesh_filter carry
 * #OPTYPE_UNDO and so push a "Global Undo" memfile step on top of their own sculpt step, which
 * makes that condition true on the very next undo -- the scene would then be reloaded from a
 * snapshot predating the entire sculpt session. Upstream keeps sculpt out of that machinery by
 * leaving #UndoType.step_foreach_ID_ref null; doing the store/resolve here preserves that while
 * keeping the #UndoRefID_Object lookup (which survives renames and same-named objects from
 * different libraries, unlike a raw `id.name` compare).
 */
static void step_store_id_refs(SculptUndoStep &us)
{
  for (std::unique_ptr<StepData> &sd : us.objects_data) {
    UndoRefID_Object &object_ref = sd->object_ref;
    if (!object_ref.ptr) {
      continue;
    }
    STRNCPY(object_ref.name, object_ref.ptr->id.name);
    if (object_ref.ptr->id.lib) {
      STRNCPY(object_ref.library_filepath_abs, object_ref.ptr->id.lib->runtime->filepath_abs);
    }
    else {
      object_ref.library_filepath_abs[0] = '\0';
    }
    /* Not needed, just prevents stale data access. */
    object_ref.ptr = nullptr;
  }
}

/**
 * Re-resolve every #StepData.object_ref back to a live pointer in the current #Main. Counterpart
 * of #step_store_id_refs; #ptr stays null for objects that were since deleted or renamed away.
 */
static void step_resolve_id_refs(SculptUndoStep &us, Main *bmain)
{
  for (std::unique_ptr<StepData> &sd : us.objects_data) {
    UndoRefID_Object &object_ref = sd->object_ref;
    ID *id = BKE_libblock_find_name_and_library_filepath(
        bmain,
        ID_OB,
        object_ref.name + 2,
        object_ref.library_filepath_abs[0] ? object_ref.library_filepath_abs : nullptr);
    object_ref.ptr = id ? id_cast<Object *>(id) : nullptr;
  }
}

static bool step_encode(bContext * /*C*/, Main *bmain, UndoStep *us_p)
{
  /* Dummy, encoding is done along the way by adding tiles
   * to the current 'SculptUndoStep' added by encode_init. */
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);

  size_t total_undo_size = 0;
  bool is_dyntopo_end = false;
  bool is_not_none = false;

  for (const std::unique_ptr<StepData> &sd : us->objects_data) {
    total_undo_size += sd->undo_size;
    if (sd->type == Type::DyntopoEnd) {
      is_dyntopo_end = true;
    }
    if (sd->type != Type::None) {
      is_not_none = true;
    }
  }

  us->step.data_size = total_undo_size;

  if (is_dyntopo_end) {
    us->step.use_memfile_step = true;
  }
  us->step.is_applied = true;

  /* We do not flush data when entering sculpt mode - this is currently indicated by Type::None */
  if (is_not_none) {
    bmain->is_memfile_undo_flush_needed = true;
  }

  step_store_id_refs(*us);

  return true;
}

static void step_decode_undo_impl(bContext *C, Depsgraph *depsgraph, SculptUndoStep *us)
{
  BLI_assert(us->step.is_applied == true);

  restore_list(C, depsgraph, us->objects_data, true);
  us->step.is_applied = false;
}

static void step_decode_redo_impl(bContext *C, Depsgraph *depsgraph, SculptUndoStep *us)
{
  BLI_assert(us->step.is_applied == false);

  restore_list(C, depsgraph, us->objects_data, false);
  us->step.is_applied = true;
}

static void step_decode_undo(bContext *C,
                             Depsgraph *depsgraph,
                             SculptUndoStep *us,
                             const bool is_final)
{
  /* Walk forward over any applied steps of same type,
   * then walk back in the next loop, un-applying them. */
  SculptUndoStep *us_iter = us;
  while (us_iter->step.next && (us_iter->step.next->type == us_iter->step.type)) {
    if (us_iter->step.next->is_applied == false) {
      break;
    }
    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.next);
  }

  while ((us_iter != us) || (!is_final && us_iter == us)) {
    BLI_assert(us_iter->step.type == us->step.type); /* Previous loop ensures this. */

    set_active_layer(C, &us_iter->active_color_start);
    step_decode_undo_impl(C, depsgraph, us_iter);

    if (us_iter == us) {
      if (us_iter->step.prev && us_iter->step.prev->type == BKE_UNDOSYS_TYPE_SCULPT) {
        set_active_layer(
            C, &reinterpret_cast<SculptUndoStep *>(us_iter->step.prev)->active_color_end);
      }
      break;
    }

    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.prev);
  }
}

static void step_decode_redo(bContext *C, Depsgraph *depsgraph, SculptUndoStep *us)
{
  SculptUndoStep *us_iter = us;
  while (us_iter->step.prev && (us_iter->step.prev->type == us_iter->step.type)) {
    if (us_iter->step.prev->is_applied == true) {
      break;
    }
    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.prev);
  }
  while (us_iter && (us_iter->step.is_applied == false)) {
    set_active_layer(C, &us_iter->active_color_end);
    step_decode_redo_impl(C, depsgraph, us_iter);

    if (us_iter == us) {
      set_active_layer(C, &us_iter->active_color_start);
      break;
    }
    us_iter = reinterpret_cast<SculptUndoStep *>(us_iter->step.next);
  }
}

static void ensure_sculpt_mode_for_object(Main *bmain,
                                          Depsgraph *depsgraph,
                                          Scene *scene,
                                          Object &ob)
{
  if (ob.type != OB_MESH) {
    return;
  }

  if (ob.mode & OB_MODE_SCULPT) {
    if (!ob.runtime->sculpt_session) {
      Mesh *mesh = id_cast<Mesh *>(ob.data);
      mesh->flag &= ~ME_SCULPT_DYNAMIC_TOPOLOGY;
      object_sculpt_mode_enter(*bmain, *depsgraph, *scene, ob, true, nullptr);
    }
    if (ob.runtime->sculpt_session) {
      ob.runtime->sculpt_session->needs_flush_to_id = true;
      ob.runtime->sculpt_session->pbvh_draw_required = true;
    }
    return;
  }

  object::mode_generic_exit(bmain, depsgraph, scene, &ob);

  /* Sculpt needs evaluated state.
   * NOTE: needs to be done here, as #object::mode_generic_exit will usually invalidate
   * (some) evaluated data. */
  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);

  Mesh *mesh = id_cast<Mesh *>(ob.data);
  /* Don't add sculpt topology undo steps when reading back undo state.
   * The undo steps must enter/exit for us. */
  mesh->flag &= ~ME_SCULPT_DYNAMIC_TOPOLOGY;
  object_sculpt_mode_enter(*bmain, *depsgraph, *scene, ob, true, nullptr);

  if (ob.runtime->sculpt_session) {
    ob.runtime->sculpt_session->needs_flush_to_id = true;
    ob.runtime->sculpt_session->pbvh_draw_required = true;
  }
}

static void step_decode(
    bContext *C, Main *bmain, UndoStep *us_p, const eUndoStepDir dir, const bool is_final)
{
  /* NOTE: behavior for undo/redo closely matches image undo. */
  BLI_assert(dir != STEP_INVALID);

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);

  /* The stored names are the only thing that survived #step_encode; turn them back into live
   * pointers into the current #Main before anything below dereferences them. */
  step_resolve_id_refs(*us, bmain);

  /* Ensure sculpt mode for all objects stored in this undo step. */
  {
    Scene *scene = CTX_data_scene(C);
    ViewLayer *view_layer = CTX_data_view_layer(C);
    BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

    bool any_sculpt_object = false;
    for (const std::unique_ptr<StepData> &sd : us->objects_data) {
      Object *ob = sd->object_ref.ptr;
      if (!ob) {
        continue;
      }
      ensure_sculpt_mode_for_object(bmain, depsgraph, scene, *ob);
      any_sculpt_object = true;
    }

    if (!any_sculpt_object) {
      Object *ob = BKE_view_layer_active_object_get(view_layer);
      if (ob && (ob->type == OB_MESH)) {
        ensure_sculpt_mode_for_object(bmain, depsgraph, scene, *ob);
        any_sculpt_object = true;
      }
    }

    if (!any_sculpt_object) {
      BLI_assert(0);
      return;
    }

    bmain->is_memfile_undo_flush_needed = true;
  }

  if (dir == STEP_UNDO) {
    step_decode_undo(C, depsgraph, us, is_final);
  }
  else if (dir == STEP_REDO) {
    step_decode_redo(C, depsgraph, us);
  }
}

static void step_free(UndoStep *us_p)
{
  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(us_p);
  for (std::unique_ptr<StepData> &sd : us->objects_data) {
    free_step_data_resources(*sd);
  }
  us->objects_data.~Vector<std::unique_ptr<StepData>>();
}

void geometry_begin(const Scene &scene, Object &ob, const wmOperator *op)
{
  geometry_begin_ex(scene, ob, op->type->name);
}

void geometry_begin_ex(const Scene & /*scene*/, Object &ob, const char *name)
{
  UndoStack *ustack = ED_undo_stack_get();

  /* If possible, we need to tag the object and its geometry data as 'changed in the future' in
   * the previous undo step if it's a memfile one. */
  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  /* Special case, we never read from this. */
  bContext *C = nullptr;

  SculptUndoStep *us = reinterpret_cast<SculptUndoStep *>(
      BKE_undosys_step_push_init_with_type(ustack, C, name, BKE_UNDOSYS_TYPE_SCULPT));
  save_common_data(ob, us);
  geometry_push(ob);
}

void geometry_begin_add_object(Object &ob)
{
  UndoStack *ustack = ED_undo_stack_get();

  ED_undosys_stack_memfile_id_changed_tag(ustack, &ob.id);
  ED_undosys_stack_memfile_id_changed_tag(ustack, ob.data);

  SculptUndoStep *us = get_init_sculpt_step();
  if (!us) {
    return;
  }
  geometry_push(ob);
}

static size_t calculate_node_geometry_allocated_size(const NodeGeometry &node_geometry)
{
  BLI_assert(node_geometry.is_initialized);

  MemoryCount memory;
  MemoryCounter memory_counter(memory);

  memory_counter.add_shared(node_geometry.face_offsets_sharing_info,
                            sizeof(int) * (node_geometry.faces_num + 1));

  CustomData_count_memory(node_geometry.corner_data, node_geometry.corners_num, memory_counter);
  CustomData_count_memory(node_geometry.face_data, node_geometry.faces_num, memory_counter);
  CustomData_count_memory(node_geometry.vert_data, node_geometry.verts_num, memory_counter);
  CustomData_count_memory(node_geometry.edge_data, node_geometry.edges_num, memory_counter);
  node_geometry.attribute_storage.wrap().count_memory(memory_counter);

  return memory.total_bytes;
}

static size_t estimate_geometry_step_size(const StepData &step_data)
{
  size_t step_size = 0;

  /* TODO: This calculation is not entirely accurate, as the current amount of memory consumed by
   * Sculpt Undo is not updated when elements are evicted. Further changes to the overall undo
   * system would be needed to measure this accurately. */
  step_size += calculate_node_geometry_allocated_size(step_data.geometry_original);
  step_size += calculate_node_geometry_allocated_size(step_data.geometry_modified);

  return step_size;
}

void geometry_end_add_object(Object &ob)
{
  geometry_push(ob);

  StepData *step_data = get_step_data(ob);
  if (step_data) {
    step_data->undo_size = estimate_geometry_step_size(*step_data);
  }
}

void geometry_end(Object &ob)
{
  geometry_end_add_object(ob);

  /* We could remove this and enforce all callers run in an operator using 'OPTYPE_UNDO'. */
  wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
  if (wm->op_undo_depth == 0) {
    UndoStack *ustack = ED_undo_stack_get();
    if (ustack->step_init) {
      BKE_undosys_step_push_with_type(ustack, nullptr, nullptr, BKE_UNDOSYS_TYPE_SCULPT);
      BKE_undosys_stack_limit_steps_and_memory_defaults(ustack);
      WM_file_tag_modified();
    }
  }
}

void register_type(UndoType *ut)
{
  ut->name = "Sculpt";
  ut->poll = nullptr; /* No poll from context for now. */
  ut->step_encode_init = step_encode_init;
  ut->step_encode = step_encode;
  ut->step_decode = step_decode;
  ut->step_free = step_free;
  /* Deliberately left null, matching upstream: registering it would opt sculpt steps into the
   * generic undo system's memfile ordering, which re-reads the whole #Main on undo. The object
   * references are stored/resolved by #step_store_id_refs / #step_resolve_id_refs instead. */
  ut->step_foreach_ID_ref = nullptr;

  ut->flags = UNDOTYPE_FLAG_DECODE_ACTIVE_STEP;

  ut->step_size = sizeof(SculptUndoStep);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Multires + Base Mesh Undo
 *
 * Example of a relevant operator is Apply Base.
 *
 * Usage:
 *
 *   static int operator_exec(bContext *C, wmOperator *op) {
 *
 *      ed::sculpt_paint::undo::push_multires_mesh_begin(C, op->type->name);
 *      // Modify base mesh.
 *      ed::sculpt_paint::undo::push_multires_mesh_end(C, op->type->name);
 *
 *      return OPERATOR_FINISHED;
 *   }
 *
 * If object is not in Sculpt mode or there is no active multires object, ED_undo_push is used
 * instead.
 * \{ */

static bool use_multires_mesh(bContext *C)
{
  if (BKE_paintmode_get_active_from_context(C) != PaintMode::Sculpt) {
    return false;
  }

  const Object *object = CTX_data_active_object(C);
  const SculptSession *sculpt_session = object->runtime->sculpt_session;

  return sculpt_session->multires_modifier;
}

void push_multires_mesh_begin(bContext *C, const char *str)
{
  if (!use_multires_mesh(C)) {
    return;
  }

  const Scene &scene = *CTX_data_scene(C);
  Object *object = CTX_data_active_object(C);

  multires_flush_sculpt_updates(object);

  push_begin_ex(scene, *object, str);

  geometry_push(*object);
}

void push_multires_mesh_end(bContext *C, const char *str)
{
  if (!use_multires_mesh(C)) {
    ED_undo_push(C, str);
    return;
  }

  Object *object = CTX_data_active_object(C);

  geometry_push(*object);

  push_end(*object);
}

/** \} */

}  // namespace ed::sculpt_paint::undo

namespace ed::sculpt_paint {

std::optional<OrigPositionData> orig_position_data_lookup_mesh_all_verts(
    const Object &object, const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(object, &node, undo::Type::Position);
  if (!unode) {
    return std::nullopt;
  }
  return OrigPositionData{unode->position.as_span(), unode->normal.as_span()};
}

std::optional<OrigPositionData> orig_position_data_lookup_mesh(const Object &object,
                                                               const bke::pbvh::MeshNode &node)
{
  const std::optional<OrigPositionData> result = orig_position_data_lookup_mesh_all_verts(object,
                                                                                          node);
  if (!result) {
    return std::nullopt;
  }
  return OrigPositionData{result->positions.take_front(node.verts().size()),
                          result->normals.take_front(node.verts().size())};
}

std::optional<OrigPositionData> orig_position_data_lookup_grids(const Object &object,
                                                                const bke::pbvh::GridsNode &node)
{
  const undo::Node *unode = undo::get_node(object, &node, undo::Type::Position);
  if (!unode) {
    return std::nullopt;
  }
  return OrigPositionData{unode->position.as_span(), unode->normal.as_span()};
}

void orig_position_data_gather_bmesh(const BMLog &bm_log,
                                     const Set<BMVert *, 0> &verts,
                                     const MutableSpan<float3> positions,
                                     const MutableSpan<float3> normals)
{
  int i = 0;
  for (const BMVert *vert : verts) {
    const float *co;
    const float *no;
    BM_log_original_vert_data(&const_cast<BMLog &>(bm_log), const_cast<BMVert *>(vert), &co, &no);
    if (!positions.is_empty()) {
      positions[i] = co;
    }
    if (!normals.is_empty()) {
      normals[i] = no;
    }
    i++;
  }
}

std::optional<Span<float4>> orig_color_data_lookup_mesh(const Object &object,
                                                        const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(object, &node, undo::Type::Color);
  if (!unode) {
    return std::nullopt;
  }
  return unode->col.as_span();
}

std::optional<Span<int>> orig_face_set_data_lookup_mesh(const Object &object,
                                                        const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(object, &node, undo::Type::FaceSet);
  if (!unode) {
    return std::nullopt;
  }
  return unode->face_sets.as_span();
}

std::optional<Span<int>> orig_face_set_data_lookup_grids(const Object &object,
                                                         const bke::pbvh::GridsNode &node)
{
  const undo::Node *unode = undo::get_node(object, &node, undo::Type::FaceSet);
  if (!unode) {
    return std::nullopt;
  }
  return unode->face_sets.as_span();
}

std::optional<Span<float>> orig_mask_data_lookup_mesh(const Object &object,
                                                      const bke::pbvh::MeshNode &node)
{
  const undo::Node *unode = undo::get_node(object, &node, undo::Type::Mask);
  if (!unode) {
    return std::nullopt;
  }
  return unode->mask.as_span();
}

std::optional<Span<float>> orig_mask_data_lookup_grids(const Object &object,
                                                       const bke::pbvh::GridsNode &node)
{
  const undo::Node *unode = undo::get_node(object, &node, undo::Type::Mask);
  if (!unode) {
    return std::nullopt;
  }
  return unode->mask.as_span();
}

}  // namespace ed::sculpt_paint

}  // namespace blender
