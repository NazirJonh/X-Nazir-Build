/* SPDX-FileCopyrightText: 2026 Modifications by Nazir
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "DNA_object_types.h"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"

/** \file
 * \ingroup bke
 *
 * Per-object sculpt history (#Object.sculpt_history), scrubbed on a timeline.
 *
 * Every sculpt edit of a mesh (each stroke, mask, filter, remesh...) adds a step. The first step
 * and every step that changes the topology (remesh, another Multires level) are keyframes holding
 * the whole shape and mask; other steps hold only the points that moved and the points whose
 * mask changed, with their exact new values. Any step is rebuilt bit-exactly from the keyframe
 * before it and the deltas after that. The points are the mesh's vertices, or with Multires in
 * Sculpt Mode the grid points at the level being sculpted.
 *
 * Making a step current (scrubbing) and then editing drops the steps after it. The history is
 * saved in the .blend file (optional), and an unreadable history is dropped with a warning
 * instead of failing the file load. All of it runs on the main thread, like the undo pushes
 * that drive it.
 *
 * ## What is Recorded
 *
 * The history keeps positions and masks only, and only of the mesh's own points (the vertices, or
 * the Multires grid points in Sculpt Mode). What that leaves out, and how each case is treated:
 * - Sculpting a shape key or through deform modifiers edits positions other than the mesh's own
 *   (the evaluated ones), which the history does not read: edits are not recorded then.
 * - Dyntopo edits a BMesh, which has no stable point indices: edits are not recorded.
 * - Face sets, paint layers and every other attribute are not part of a step: an edit that only
 *   touches them adds no step (a step carries no change, see #record).
 * - The SCULPT_LAYERS coefficients are not restored by a step: a step restores the composed
 *   positions, so scrubbing shows the right shape but leaves the layer coefficients as they are
 *   now (an edit on an older step therefore does not replay the layer edits of the dropped
 *   steps either). SculptLayer operations (merge, flatten, bake) do record history steps because
 *   they change the composed positions.
 *
 * ## Relationship with Undo
 *
 * The editor side owns the synchronization contract ("Undo Stack and Sculpt History
 * Synchronization", next to the undo step data). In summary:
 * - Every history step corresponds to an undo step
 * - Undo/redo moves the history cursor via the #UndoType history hooks
 *   (#UndoTypeHistoryHooks in `BKE_undo_system.hh`)
 * - The undo stack is authoritative; history follows it
 * - #is_at_current tracks whether the mesh matches the current history step bit-exactly
 */
namespace blender {
struct BlendDataReader;
struct BlendWriter;
struct Main;
}  // namespace blender

namespace blender::bke {

/**
 * What the points of a step are: the mesh's vertices (with its faces), or Multires grid points.
 * A step can only be applied to a mesh of the same layout.
 */
struct StepLayout {
  int points_num = 0;
  int faces_num = 0;
  int corners_num = 0;
  /** Grids: their number and size (0 for a mesh). */
  int grids_num = 0;
  int grid_size = 0;

  bool is_grids() const
  {
    return grids_num > 0;
  }
  bool operator==(const StepLayout &other) const = default;
};

/** What #SculptHistory.runtime points to: the shape and mask at #SculptHistory.current. */
struct SculptHistoryRuntime {
  Array<float3> positions;
  Array<float> masks;
  StepLayout layout;
};

namespace sculpt_history {

/**
 * The cursor #cursor_get returns for an object that tracks no history step; #cursor_set of it
 * (or of a step that has since been dropped) does nothing.
 */
constexpr int NO_CURSOR = 0;

/** The cursor of a step: #SculptHistoryStep.number plus one (0 is "no history"). */
inline int step_cursor(const SculptHistoryStep &step)
{
  return step.number + 1;
}

/** Give the object a history (recording, saved in the file) if it has none; returns it. */
SculptHistory &ensure(Object &ob);
/** Delete the object's history entirely. */
void remove(Object &ob);
void free(SculptHistory *history);

/** Whether edits of this object are recorded now (the sculpt history preference, for meshes). */
bool is_recording(const Object &ob);

/** Steps kept for each object (the preference); 0 keeps them all. */
int step_limit();
/** Apply the step limit to every object's history (after the preference changed). */
void apply_limit_to_all(Main &bmain);

/**
 * The current step as a stable number (#step_cursor), for undo steps to remember where the
 * history was.
 */
int cursor_get(const Object &ob);
/**
 * Make the step `cursor` (from #cursor_get) current without touching the mesh: undo and redo
 * have just put the mesh in that state themselves. Steps since dropped are skipped.
 */
void cursor_set(Object &ob, int cursor);

/**
 * Before an edit: make the mesh as it is now the first step, if there is none yet. Nothing is
 * recorded while a #ScopedPause is alive (history operations restoring steps).
 */
void begin_edit(Object &ob, StringRef name);
/**
 * After an edit: add a step with what changed since the current step (nothing when nothing
 * moved). `topology_changed` makes it a keyframe. Steps after the current one are dropped first,
 * and the oldest ones after, past the limit.
 *
 * \param candidate_points: the only points that could have changed, when known (the undo step's
 * touched ones). Empty means unknown: every point is compared. Points outside a mesh of the
 * recorded layout are ignored.
 */
void record(Object &ob,
            StringRef name,
            bool topology_changed,
            Span<int> candidate_points = {});

/** While alive, #begin_edit and #record do nothing for \a ob. */
class ScopedPause {
 public:
  ScopedPause(Object &ob);
  ~ScopedPause();

 private:
  Object &ob_;
};

/** The shape and mask at a step. */
struct StepShape {
  Array<float3> positions;
  Array<float> masks;
  /** The keyframe the step builds on, which has its topology. */
  int keyframe = 0;
};
StepShape step_shape(const SculptHistory &history, int index);

/** Whether anything is masked at step `index`. */
bool step_has_mask(const SculptHistory &history, int index);

/**
 * Whether the mesh (or its Multires grids in Sculpt Mode) is exactly in the shape and mask of the
 * current step. Undo can put it elsewhere: reloading a whole-scene step from before the history
 * moved without undo steps.
 */
bool is_at_current(const Object &ob);

/** The index of the step with this cursor (#cursor_get), or -1 when it is gone. */
int find_cursor(const SculptHistory &history, int cursor);

enum class ApplyResult {
  /** The step's positions and masks were written in place (the caller updates the session). */
  Positions,
  /** The mesh was replaced with the step's topology. Attributes whose domain matches the step's
   * topology are carried over (vertex groups and colors on a same vertex count, UVs and face sets
   * when the faces are the same); the rest is dropped, like after a remesh. */
  Topology,
  /**
   * Nothing changed: the step is of Multires grids at another subdivision level, or of the mesh
   * while Multires is sculpted (or the other way around), or it is masked and the grids have no
   * mask layer yet.
   */
  Failed,
};

/**
 * Put the mesh (or, for Multires in Sculpt Mode, its grids) in the state of step `index` and make
 * it current.
 */
ApplyResult apply_step(Object &ob, int index);

/** Delete the steps after the current one (sculpting from an older step replaces them). */
void truncate_after_current(SculptHistory &history);

/** Drop the oldest steps past the limit, never the current one. */
void apply_limit(SculptHistory &history);

int64_t memory_bytes(const SculptHistory &history);

void blend_write(BlendWriter &writer, const SculptHistory &history);
/** Reads the history of `ob`; an invalid one is removed with a warning. */
void blend_read(BlendDataReader &reader, Object &ob);

}  // namespace sculpt_history
}  // namespace blender::bke
