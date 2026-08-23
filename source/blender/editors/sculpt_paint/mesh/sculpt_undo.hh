/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <cstdint>

#include "BLI_index_mask_fwd.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
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
  /** Poly Paint: one or more named `CD_PROP_FLOAT` point attributes (metallic, roughness,
   * specular, and/or a custom attribute), as selected by PaintModeSettings when the step
   * was pushed. All attribute names for the step are stored together; each undo node holds
   * an aligned, type-erased snapshot for every name (see #undo::StepData::material_attributes).
   * Color-shaped channel attributes (e.g. Base Color) are snapshotted in the same Material step
   * via #MaterialUndoAttributes::color_names. */
  Material,
};

struct StepData;

/**
 * Poly Paint: the attributes a #Type::Material step covers, as one aggregate rather than a run of
 * same-typed name lists - the fields cannot be silently swapped at a call site, and a future
 * channel shape is a new field instead of a new positional parameter.
 *
 * The whole set is fixed by the first push of a step and every later push of that step must pass
 * an identical one (asserted): the per-node buffers are indexed positionally against it, and
 * #ensure_node does not refill a node that was already stored, so a changing list would silently
 * misalign the restore.
 */
struct MaterialUndoAttributes {
  /** Point float attribute names to snapshot. */
  Span<StringRef> scalar_names;
  /** Color attribute names to snapshot in the same step, name-keyed like \a scalar_names. */
  Span<StringRef> color_names;
  /**
   * Subset of the two lists above that the current stroke itself created (didn't exist before it
   * started). Undoing the step removes these attributes instead of leaving a zero-valued one
   * behind; redoing recreates them before the per-node buffers are swapped back.
   */
  Span<StringRef> created_names;
};

/**
 * Store undo data of the given type for a pbvh::Tree node. This function can be called by multiple
 * threads concurrently, as long as they don't pass the same pbvh::Tree node.
 *
 * This is only possible when building an undo step, in between #push_begin and #push_end.
 *
 * \param material_attributes: Required (and used) only when \a type is #Type::Material.
 */
void push_node(const Depsgraph &depsgraph,
               const Object &object,
               const bke::pbvh::Node *node,
               undo::Type type,
               const MaterialUndoAttributes &material_attributes = {});
void push_nodes(const Depsgraph &depsgraph,
                Object &object,
                const IndexMask &node_mask,
                undo::Type type,
                const MaterialUndoAttributes &material_attributes = {});

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
/**
 * Take the sculpt mask back to what the open undo step recorded. The mask counterpart of
 * #restore_position_from_undo_step; both exist so a caller that knows which TARGET a stroke wrote
 * can revert exactly that one, the way `restore_from_undo_step()` dispatches internally.
 */
void restore_mask_from_undo_step(Object &object);

/**
 * Poly Paint: rolls every attribute covered by the in-progress #Type::Material step back to its
 * pre-stroke snapshot, for anchored and drag-dot strokes that re-apply the whole dab each time
 * the cursor moves.
 *
 * Unlike the undo/redo path this copies instead of swapping, so the snapshot stays valid for the
 * next dab of the same stroke - the same contract #restore_color_from_undo_step has for
 * #Type::Color steps.
 *
 * \return false when the current step is not a #Type::Material step, so the caller can fall back
 * to the color-only restore.
 */
bool restore_material_attributes_from_step(Object &object);

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
