/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <cstdint>

#include "BLI_index_mask_fwd.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct bContext;
struct Depsgraph;
struct Mesh;
struct Object;
struct Scene;
struct wmOperator;
namespace bke::pbvh {
class Node;
}
namespace ed::sculpt_paint {
/* Fully declared in `sculpt_intern.hh`; forward-declared here (no explicit enum-base, matching
 * the real definition's implicit `int` underlying type) so this header does not have to pull in
 * `sculpt_intern.hh`'s heavier dependencies just for #finish_multi_object's parameter type. */
enum class UpdateType;
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
void push_enter_sculpt_mode_add_object(Object &ob);

/**
 * NOTE: #push_begin is preferred since `name`
 * must match operator name for redo panels to work.
 */
void push_begin_ex(const Scene &scene, Object &ob, const char *name);
void push_begin_add_object(Object &ob);
void push_end(Object &ob);
void push_end_ex(Object &ob, bool use_nested_undo, bool finalize_undo_step = true);
/** Finalize undo data for all objects in the active sculpt undo step and optionally push it. */
void push_end_all_ex(bool use_nested_undo, bool finalize_undo_step = true);
/** Discard the sculpt undo step currently being built without pushing it to the stack. */
void discard_init_step();

/**
 * Add another object's full-geometry snapshot to the multi-object step opened by #geometry_begin,
 * mirroring how #push_begin_add_object extends the step opened by #push_begin. A no-op if no
 * step is currently pending (mirrors #push_begin_add_object's own guard).
 */
void geometry_begin_add_object(Object &ob);

/**
 * Capture \a ob's geometry snapshot into the currently open multi-object step, WITHOUT
 * finalizing/pushing the step (unlike #geometry_end, which does both -- calling #geometry_end
 * per object in a multi-object gesture pushes/clears the step after the FIRST object, leaving
 * every subsequent #geometry_push call with no pending step to write into). Call
 * #push_end_all_ex(false, true) once, after every object has been captured this way, to finalize
 * and push the whole multi-object step (mirrors how #finish_multi_object closes the per-node
 * multi-object undo path).
 */
void geometry_end_add_object(Object &ob);

void restore_from_bmesh_enter_geometry(const StepData &step_data, Mesh &mesh);
bool has_bmesh_log_entry(const Object &ob);

void restore_position_from_undo_step(const Depsgraph &depsgraph, Object &object);

/**
 * Snapshot \a ob's current #Object::object_to_world() into the currently-open sculpt undo step
 * (the one opened by #push_begin_ex/#push_begin_add_object), so a later Ctrl+Z can restore it.
 * Used by Origin Correct: a rigid-body secondary object's own matrix, not its mesh, is what needs
 * undo coverage. A no-op if no step is currently pending (mirrors #push_begin_add_object's own
 * guard).
 */
void set_object_transform_snapshot(Object &ob);

/**
 * Re-apply \a ob's object-transform snapshot captured by #set_object_transform_snapshot, WITHOUT
 * consuming/swapping it (unlike the Ctrl+Z stack-level restore in `restore_list_object`) -- safe to
 * call repeatedly mid-session. Used by #cancel_modal_transform to revert a rigid-body secondary to
 * its pre-session matrix. A no-op if no snapshot was captured for \a ob.
 */
void restore_object_transform_from_undo_step(Object &ob);

/**
 * Read (without consuming or mutating) the object-transform snapshot captured by
 * #set_object_transform_snapshot for \a ob, if one exists in the currently-open undo step.
 * Unlike #restore_object_transform_from_undo_step, this never touches \a ob itself -- used by a
 * rigid-body Origin Correct secondary's per-step math, which needs the FIXED session-start matrix
 * as a value to compute with, not applied to the object (that happens separately via
 * #BKE_object_apply_mat4). Returns false (leaving \a r_transform untouched) if no snapshot was
 * captured for \a ob.
 */
bool get_object_transform_snapshot(const Object &ob, float4x4 &r_transform);

/* -------------------------------------------------------------------- */
/** \name Multi-object ("global") sculpt undo helpers
 *
 * Unify the ≥10 repeated patterns across sculpt_mask, sculpt_face_set, sculpt_filter_mask,
 * sculpt_filter_color, sculpt_mask_init, sculpt_ops (mask_by_color), sculpt_expand (paste),
 * paint_mask (flood_fill + gesture_begin) of:
 *
 *     undo::push_begin(scene, primary_ob, op);
 *     for (Object *ob : objects) {
 *       if (ob != primary_ob) undo::push_begin_add_object(*ob);
 *     }
 *     ... operator body ...
 *     undo::push_end_all_ex(false, true);
 *     for (Object *ob : objects) {
 *       flush_update_done(C, *ob, UpdateType::X);
 *       WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
 *     }
 * \{ */

/**
 * Open an undo step covering the multi-object ("global") sculpt operation rooted at the first
 * object in \a scene_objects (the active sculpt object, per
 * #ed::sculpt_paint::sculpt_mode_objects(vc)[0]). Calls #push_begin for that object, then
 * #push_begin_add_object for every remaining entry.
 *
 * A no-op when \a scene_objects is empty. The caller is still responsible for doing meaningful
 * work between this and #finish_multi_object; #finish_multi_object will call #push_end_all_ex
 * regardless of whether the operator body touched any object -- so if the operator's per-object
 * work all bailed out before mutating anything, prefer #discard_init_step instead.
 */
void push_begin_multi_object(const Scene &scene,
                             const wmOperator *op,
                             Span<Object *> scene_objects);

/**
 * Close the multi-object undo step opened by #push_begin_multi_object, publish a per-object
 * viewport redraw and dependency-graph tag at the requested #ed::sculpt_paint::UpdateType, and
 * fire one `NC_OBJECT | ND_DRAW` notifier per object so every viewport redraws.
 *
 * Pass `bContext *C` already in the operator's `exec` scope. \a update_type must match the
 * field that the operator actually mutated (mask / face-set / color / position / etc.); the brush
 * helpers will route the tag to the correct update graph path.
 */
void finish_multi_object(bContext *C,
                         Span<Object *> scene_objects,
                         UpdateType update_type);

/** \} */

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
