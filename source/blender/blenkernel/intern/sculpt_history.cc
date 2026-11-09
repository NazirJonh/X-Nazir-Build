/* SPDX-FileCopyrightText: 2026 Modifications by Nazir
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "BLI_index_mask.hh"
#include "BLI_listbase.h"
#include "BLI_string_ref.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_filters.hh"
#include "BKE_customdata.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh.h"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_sculpt_history.hh"
#include "BKE_report.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLO_read_write.hh"

namespace blender::bke::sculpt_history {

/** The mask attribute sculpting writes. */
static constexpr StringRef MASK_ATTRIBUTE = ".sculpt_mask";
/** The face set attribute (user data, so it is carried over a topology replacement). */
static constexpr StringRef FACE_SET_ATTRIBUTE = ".sculpt_face_set";

/** Where the object's sculpted points are now: its mesh, or its Multires grids in Sculpt Mode. */
struct State {
  StepLayout layout;
  Span<float3> positions;
  /** Empty when nothing is masked. */
  Span<float> masks;
  /** Owns the mesh's masks for `masks`. */
  VArraySpan<float> mesh_masks;
  const Mesh *mesh = nullptr;
  SubdivCCG *subdiv_ccg = nullptr;
};

static SubdivCCG *object_grids(const Object &ob)
{
  const SculptSession *ss = ob.runtime->sculpt_session;
  if (ob.mode & OB_MODE_SCULPT && ss && ss->bm == nullptr && ss->subdiv_ccg &&
      ss->subdiv_ccg->grids_num > 0)
  {
    return ss->subdiv_ccg;
  }
  return nullptr;
}

static bool read_state(const Object &ob, State &state)
{
  if (ob.type != OB_MESH) {
    return false;
  }
  if (SubdivCCG *subdiv_ccg = object_grids(ob)) {
    state.subdiv_ccg = subdiv_ccg;
    state.positions = subdiv_ccg->positions;
    state.masks = subdiv_ccg->masks;
    state.layout = StepLayout{int(state.positions.size()),
                              0,
                              0,
                              subdiv_ccg->grids_num,
                              subdiv_ccg->grid_size};
    return true;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  state.mesh = &mesh;
  state.positions = mesh.vert_positions();
  if (const VArray<float> masks = *mesh.attributes().lookup<float>(MASK_ATTRIBUTE,
                                                                   AttrDomain::Point))
  {
    state.mesh_masks = masks;
    state.masks = state.mesh_masks;
  }
  state.layout = StepLayout{mesh.verts_num, mesh.faces_num, mesh.corners_num, 0, 0};
  return true;
}

static float mask_at(const Span<float> masks, const int64_t i)
{
  return masks.is_empty() ? 0.0f : masks[i];
}

static bool is_paused(const Object &ob)
{
  return ob.runtime->sculpt_history_pause_count > 0;
}

ScopedPause::ScopedPause(Object &ob) : ob_(ob)
{
  ob_.runtime->sculpt_history_pause_count++;
}

ScopedPause::~ScopedPause()
{
  ob_.runtime->sculpt_history_pause_count--;
}

static Span<float3> step_positions(const SculptHistoryStep &step)
{
  return {reinterpret_cast<const float3 *>(step.positions), step.verts_num};
}

static void free_runtime(SculptHistory &history)
{
  MEM_delete(history.runtime);
  history.runtime = nullptr;
}

static void free_step(SculptHistoryStep &step)
{
  MEM_SAFE_DELETE(step.positions);
  MEM_SAFE_DELETE(step.face_offsets);
  MEM_SAFE_DELETE(step.corner_verts);
  MEM_SAFE_DELETE(step.delta_indices);
  MEM_SAFE_DELETE(step.delta_positions);
  MEM_SAFE_DELETE(step.masks);
  MEM_SAFE_DELETE(step.mask_delta_indices);
  MEM_SAFE_DELETE(step.mask_delta_values);
}

void free(SculptHistory *history)
{
  if (history == nullptr) {
    return;
  }
  for (const int i : IndexRange(history->steps_num)) {
    free_step(history->steps[i]);
  }
  MEM_SAFE_DELETE(history->steps);
  free_runtime(*history);
  MEM_delete(history);
}

SculptHistory &ensure(Object &ob)
{
  if (ob.sculpt_history == nullptr) {
    ob.sculpt_history = MEM_new<SculptHistory>(__func__);
  }
  return *ob.sculpt_history;
}

void remove(Object &ob)
{
  free(ob.sculpt_history);
  ob.sculpt_history = nullptr;
}

bool is_recording(const Object &ob)
{
  return !U.sculpt_history_off && ob.type == OB_MESH;
}

int step_limit()
{
  /* Zero (preferences from before the setting) is the default; -1 keeps every step. */
  if (U.sculpt_history_limit == 0) {
    return 100;
  }
  return U.sculpt_history_limit < 0 ? 0 : U.sculpt_history_limit;
}

void apply_limit_to_all(Main &bmain)
{
  for (Object &ob : bmain.objects) {
    if (ob.sculpt_history) {
      apply_limit(*ob.sculpt_history);
    }
  }
}

int cursor_get(const Object &ob)
{
  const SculptHistory *history = ob.sculpt_history;
  if (history == nullptr || history->steps_num == 0) {
    return NO_CURSOR;
  }
  return step_cursor(history->steps[history->current]);
}

void cursor_set(Object &ob, const int cursor)
{
  SculptHistory *history = ob.sculpt_history;
  if (history == nullptr || cursor == NO_CURSOR) {
    return;
  }
  for (const int i : IndexRange(history->steps_num)) {
    if (step_cursor(history->steps[i]) == cursor) {
      if (i != history->current) {
        history->current = i;
        /* The compared-against shape is rebuilt from the new current step when needed. */
        free_runtime(*history);
      }
      return;
    }
  }
}

static int keyframe_of(const SculptHistory &history, const int index)
{
  for (int i = index; i > 0; i--) {
    if (history.steps[i].flag & SCULPT_HISTORY_STEP_KEYFRAME) {
      return i;
    }
  }
  return 0;
}

template<typename T>
static void scatter(const Span<int> indices, const Span<T> values, MutableSpan<T> dst)
{
  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int j : range) {
      dst[indices[j]] = values[j];
    }
  });
}

static Span<float3> delta_positions(const SculptHistoryStep &step)
{
  return {reinterpret_cast<const float3 *>(step.delta_positions), step.delta_num};
}

static StepLayout step_layout(const SculptHistoryStep &keyframe)
{
  return StepLayout{keyframe.verts_num,
                    keyframe.faces_num,
                    keyframe.corners_num,
                    keyframe.grids_num,
                    keyframe.grid_size};
}

StepShape step_shape(const SculptHistory &history, const int index)
{
  StepShape shape;
  shape.keyframe = keyframe_of(history, index);
  const SculptHistoryStep &keyframe = history.steps[shape.keyframe];

  /* The runtime holds the shape of the current step; starting from it skips re-gathering the
   * keyframe and every step before, which is most of the work when scrubbing forward. It can
   * only stand in when the step builds on the same keyframe the runtime was made at (the deltas
   * in between belong to that chain, and a keyframe step in between would have to reset it).
   * The shortcut is only valid forward (index >= current): going backward requires rebuilding
   * from the keyframe, because the runtime contains deltas that need to be un-applied, and we
   * store only forward deltas. */
  const SculptHistoryRuntime *runtime = history.runtime;
  int first_delta = shape.keyframe + 1;
  if (runtime != nullptr && runtime->layout == step_layout(keyframe) &&
      index >= history.current &&
      keyframe_of(history, history.current) == shape.keyframe)
  {
    shape.positions = runtime->positions;
    shape.masks = runtime->masks;
    first_delta = history.current + 1;
  }
  if (shape.positions.is_empty()) {
    shape.positions = step_positions(keyframe);
    shape.masks.reinitialize(keyframe.verts_num);
    if (keyframe.masks) {
      shape.masks.as_mutable_span().copy_from({keyframe.masks, keyframe.verts_num});
    }
    else {
      shape.masks.fill(0.0f);
    }
  }
  for (int i = first_delta; i <= index; i++) {
    const SculptHistoryStep &step = history.steps[i];
    scatter({step.delta_indices, step.delta_num},
            delta_positions(step),
            shape.positions.as_mutable_span());
    scatter<float>({step.mask_delta_indices, step.mask_delta_num},
                   {step.mask_delta_values, step.mask_delta_num},
                   shape.masks);
  }
  return shape;
}

static void set_runtime(SculptHistory &history, StepShape &&shape, const StepLayout &layout)
{
  free_runtime(history);
  SculptHistoryRuntime *runtime = MEM_new<SculptHistoryRuntime>(__func__);
  runtime->positions = std::move(shape.positions);
  runtime->masks = std::move(shape.masks);
  runtime->layout = layout;
  history.runtime = runtime;
}

static SculptHistoryRuntime &ensure_runtime(SculptHistory &history)
{
  if (history.runtime == nullptr) {
    StepShape shape = step_shape(history, history.current);
    const StepLayout layout = step_layout(history.steps[shape.keyframe]);
    set_runtime(history, std::move(shape), layout);
  }
  return *history.runtime;
}

/** Appends an empty step, reusing the capacity kept by the dropped steps when there is any. */
static SculptHistoryStep &append_step(SculptHistory &history, const StringRef name)
{
  if (history.steps_num == history.steps_capacity) {
    /* Doubling keeps a long history from reallocating (and copying every step) at each edit. */
    const int new_capacity = std::max(history.steps_capacity * 2, 8);
    history.steps = static_cast<SculptHistoryStep *>(
        MEM_realloc_uninitialized(history.steps, sizeof(SculptHistoryStep) * size_t(new_capacity)));
    history.steps_capacity = new_capacity;
  }
  SculptHistoryStep &step = history.steps[history.steps_num++];
  step = SculptHistoryStep{};
  step.number = history.next_number++;
  name.copy_utf8_truncated(step.name);
  return step;
}

/** Removes step `index`, already freed. Its slot is kept for the next appended step. */
static void erase_step(SculptHistory &history, const int index)
{
  memmove(history.steps + index,
          history.steps + index + 1,
          sizeof(SculptHistoryStep) * (history.steps_num - index - 1));
  history.steps_num--;
}

static bool any_masked(const Span<float> masks)
{
  return std::any_of(masks.begin(), masks.end(), [](const float m) { return m != 0.0f; });
}

static void set_keyframe(SculptHistoryStep &step, const State &state)
{
  const StepLayout &layout = state.layout;
  step.flag |= SCULPT_HISTORY_STEP_KEYFRAME;
  step.verts_num = layout.points_num;
  step.faces_num = layout.faces_num;
  step.corners_num = layout.corners_num;
  step.grids_num = layout.grids_num;
  step.grid_size = layout.grid_size;
  step.positions = MEM_new_array_uninitialized<float>(size_t(layout.points_num) * 3, __func__);
  MutableSpan<float>(step.positions, int64_t(layout.points_num) * 3)
      .copy_from(state.positions.cast<float>());
  step.face_offsets = MEM_new_array_uninitialized<int>(size_t(layout.faces_num) + 1, __func__);
  step.face_offsets[0] = 0;
  step.corner_verts = MEM_new_array_uninitialized<int>(size_t(layout.corners_num), __func__);
  if (state.mesh) {
    if (layout.faces_num > 0) {
      memcpy(step.face_offsets,
             state.mesh->face_offsets().data(),
             sizeof(int) * (layout.faces_num + 1));
    }
    if (layout.corners_num > 0) {
      memcpy(step.corner_verts, state.mesh->corner_verts().data(), sizeof(int) * layout.corners_num);
    }
  }
  if (any_masked(state.masks)) {
    step.masks = MEM_new_array_uninitialized<float>(size_t(layout.points_num), __func__);
    MutableSpan<float>(step.masks, layout.points_num).copy_from(state.masks);
  }
}

static void set_runtime_from_state(SculptHistory &history, const State &state)
{
  StepShape shape;
  shape.positions = state.positions;
  shape.masks.reinitialize(state.layout.points_num);
  if (state.masks.is_empty()) {
    shape.masks.fill(0.0f);
  }
  else {
    shape.masks.as_mutable_span().copy_from(state.masks);
  }
  set_runtime(history, std::move(shape), state.layout);
}

void begin_edit(Object &ob, const StringRef /*name*/)
{
  if (is_paused(ob) || !is_recording(ob)) {
    return;
  }
  /* The first edit of an object starts its history. */
  if (ob.sculpt_history && ob.sculpt_history->steps_num > 0) {
    return;
  }
  State state;
  if (!read_state(ob, state)) {
    return;
  }
  SculptHistory &history = ensure(ob);
  set_keyframe(append_step(history, IFACE_("Start")), state);
  history.current = 0;
  set_runtime_from_state(history, state);
}

void truncate_after_current(SculptHistory &history)
{
  if (history.current >= history.steps_num - 1) {
    return;
  }
  for (int i = history.current + 1; i < history.steps_num; i++) {
    free_step(history.steps[i]);
  }
  /* The slots of the dropped steps are kept: appending reuses them, so the usual edit after a
   * scrub does not reallocate the array. */
  history.steps_num = history.current + 1;
}

void apply_limit(SculptHistory &history)
{
  const int limit = step_limit();
  if (limit <= 0) {
    return;
  }
  while (history.steps_num > std::max(limit, 1) && history.current > 0) {
    SculptHistoryStep &first = history.steps[0];
    SculptHistoryStep &second = history.steps[1];
    if (second.flag & SCULPT_HISTORY_STEP_KEYFRAME) {
      free_step(first);
      erase_step(history, 0);
    }
    else {
      /* Fold the second step into the first keyframe, which then has its shape and mask. */
      scatter({second.delta_indices, second.delta_num},
              delta_positions(second),
              MutableSpan<float3>(reinterpret_cast<float3 *>(first.positions), first.verts_num));
      if (second.mask_delta_num > 0) {
        if (first.masks == nullptr) {
          first.masks = MEM_new_array<float>(size_t(first.verts_num), __func__);
        }
        scatter<float>({second.mask_delta_indices, second.mask_delta_num},
                       {second.mask_delta_values, second.mask_delta_num},
                       {first.masks, first.verts_num});
      }
      first.number = second.number;
      memcpy(first.name, second.name, sizeof(first.name));
      free_step(second);
      erase_step(history, 1);
    }
    history.current--;
  }
}

void record(Object &ob,
            const StringRef name,
            bool topology_changed,
            const Span<int> candidate_points)
{
  if (is_paused(ob) || !is_recording(ob)) {
    return;
  }
  State state;
  if (!read_state(ob, state)) {
    return;
  }
  SculptHistory &history = ensure(ob);
  if (history.steps_num == 0) {
    /* Nothing to compare to: the mesh as it is now is where the history starts. */
    set_keyframe(append_step(history, name), state);
    history.current = 0;
    set_runtime_from_state(history, state);
    return;
  }

  SculptHistoryRuntime &runtime = ensure_runtime(history);
  const Span<float3> positions = state.positions;
  topology_changed |= state.layout != runtime.layout;

  /* The mesh point each compared position belongs to: the undo step's touched points when they
   * are known, or every point. */
  const auto mesh_point = [&](const int64_t pos) {
    return candidate_points.is_empty() ? int(pos) : candidate_points[pos];
  };
  const IndexRange to_compare = candidate_points.is_empty() ? positions.index_range() :
                                                              IndexRange(candidate_points.size());

  IndexMaskMemory memory;
  IndexMask moved;
  IndexMask remasked;
  if (!topology_changed) {
    /* Compare bits, so the step restores exactly. #from_predicate runs in parallel, which keeps
     * the full pass affordable on a big mesh. */
    moved = IndexMask::from_predicate(to_compare, memory, [&](const int64_t pos) {
      const int i = mesh_point(pos);
      return memcmp(&positions[i], &runtime.positions[i], sizeof(float3)) != 0;
    });
    remasked = IndexMask::from_predicate(to_compare, memory, [&](const int64_t pos) {
      const int i = mesh_point(pos);
      const float mask = mask_at(state.masks, i);
      return memcmp(&mask, &runtime.masks[i], sizeof(float)) != 0;
    });
    if (moved.is_empty() && remasked.is_empty()) {
      /* A face set or visibility edit: nothing the history keeps. */
      return;
    }
  }

  /* Editing from the middle of the history drops the steps after it. */
  truncate_after_current(history);

  SculptHistoryStep &step = append_step(history, name);
  if (topology_changed) {
    set_keyframe(step, state);
    set_runtime_from_state(history, state);
  }
  else {
    step.delta_num = int(moved.size());
    step.delta_indices = MEM_new_array_uninitialized<int>(moved.size(), __func__);
    step.delta_positions = MEM_new_array_uninitialized<float>(moved.size() * 3, __func__);
    float3 *values = reinterpret_cast<float3 *>(step.delta_positions);
    moved.foreach_index([&](const int64_t pos, const int64_t out) {
      const int i = mesh_point(pos);
      values[out] = positions[i];
      runtime.positions[i] = positions[i];
      step.delta_indices[out] = i;
    });
    if (!remasked.is_empty()) {
      step.mask_delta_num = int(remasked.size());
      step.mask_delta_indices = MEM_new_array_uninitialized<int>(remasked.size(), __func__);
      step.mask_delta_values = MEM_new_array_uninitialized<float>(remasked.size(), __func__);
      remasked.foreach_index([&](const int64_t pos, const int64_t out) {
        const int i = mesh_point(pos);
        step.mask_delta_values[out] = mask_at(state.masks, i);
        runtime.masks[i] = step.mask_delta_values[out];
        step.mask_delta_indices[out] = i;
      });
    }
  }
  history.current = history.steps_num - 1;
  apply_limit(history);
}

bool step_has_mask(const SculptHistory &history, const int index)
{
  const int keyframe = keyframe_of(history, index);
  if (history.steps[keyframe].masks) {
    return true;
  }
  for (int i = keyframe + 1; i <= index; i++) {
    if (history.steps[i].mask_delta_num > 0) {
      return true;
    }
  }
  return false;
}

static void write_mesh_masks(Mesh &mesh, const Span<float> masks)
{
  MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (!any_masked(masks)) {
    attributes.remove(MASK_ATTRIBUTE);
    return;
  }
  SpanAttributeWriter<float> writer = attributes.lookup_or_add_for_write_only_span<float>(
      MASK_ATTRIBUTE, AttrDomain::Point);
  writer.span.copy_from(masks);
  writer.finish();
}

/** Whether the mesh's faces are exactly the keyframe's (same counts, offsets and corners). */
static bool mesh_faces_match(const Mesh &mesh, const SculptHistoryStep &keyframe)
{
  if (mesh.faces_num != keyframe.faces_num || mesh.corners_num != keyframe.corners_num) {
    return false;
  }
  if (keyframe.faces_num == 0) {
    return true;
  }
  return std::equal(mesh.face_offsets().data(),
                    mesh.face_offsets().data() + mesh.faces_num + 1,
                    keyframe.face_offsets) &&
         std::equal(mesh.corner_verts().data(),
                    mesh.corner_verts().data() + mesh.corners_num,
                    keyframe.corner_verts);
}

/**
 * Carries the attributes of the replaced mesh over to the mesh of the step's topology. Vertex-bound
 * data keeps its meaning where the vertex count matches (the indices then name the same vertices),
 * face-bound data where the faces are the keyframe's exactly. Everywhere else the elements have no
 * correspondence — a remesh's vertices are new ones — and guessing a mapping would corrupt the
 * attributes silently, so those are dropped instead. The sculpt layer tree is not carried here on
 * purpose: #BKE_mesh_nomain_to_mesh owns that policy (it keeps the tree on a same-vertex-count
 * replacement and drops it with a warning otherwise, like every other geometry replacement).
 */
static void carry_over_attributes(Mesh &mesh, Mesh &new_mesh, const bool verts, const bool faces)
{
  if (verts) {
    /* The group names travel with the vertex group data copied below. */
    std::swap(new_mesh.vertex_group_names, mesh.vertex_group_names);
    std::swap(new_mesh.active_color_attribute, mesh.active_color_attribute);
    std::swap(new_mesh.default_color_attribute, mesh.default_color_attribute);
  }
  if (faces) {
    std::swap(new_mesh.active_uv_map_attribute, mesh.active_uv_map_attribute);
    std::swap(new_mesh.default_uv_map_attribute, mesh.default_uv_map_attribute);
    std::swap(new_mesh.stencil_uv_map_attribute, mesh.stencil_uv_map_attribute);
    std::swap(new_mesh.clone_uv_map_attribute, mesh.clone_uv_map_attribute);
  }

  /* The mask is written from the step; positions are the step's; internal attributes (topology,
   * selection) have no user to miss them. Face sets are user data and travel with the faces. */
  const AttributeFilterFromFunc filter([](const StringRef name) {
    if (name == "position" || name == MASK_ATTRIBUTE ||
        (name.startswith(".") && name != FACE_SET_ATTRIBUTE))
    {
      return AttributeFilter::Result::AllowSkip;
    }
    return AttributeFilter::Result::Process;
  });
  const AttributeAccessor src = mesh.attributes();
  MutableAttributeAccessor dst = new_mesh.attributes_for_write();
  if (verts) {
    copy_attributes(src, AttrDomain::Point, AttrDomain::Point, filter, dst);
  }
  if (faces) {
    copy_attributes(src, AttrDomain::Face, AttrDomain::Face, filter, dst);
    copy_attributes(src, AttrDomain::Corner, AttrDomain::Corner, filter, dst);
  }
}

ApplyResult apply_step(Object &ob, int index)
{
  SculptHistory *history = ob.sculpt_history;
  State state;
  if (history == nullptr || history->steps_num == 0 || !read_state(ob, state)) {
    return ApplyResult::Failed;
  }
  index = std::clamp(index, 0, history->steps_num - 1);
  const int keyframe_index = keyframe_of(*history, index);
  const SculptHistoryStep &keyframe = history->steps[keyframe_index];
  const StepLayout layout = step_layout(keyframe);

  ApplyResult result = ApplyResult::Positions;
  if (layout.is_grids() || state.layout.is_grids()) {
    /* Grids are written in place: the step must be at the subdivision level being sculpted. */
    if (layout != state.layout) {
      return ApplyResult::Failed;
    }
    if (step_has_mask(*history, index) && state.subdiv_ccg->masks.is_empty()) {
      /* The caller adds the grids' mask layer first. */
      return ApplyResult::Failed;
    }
  }
  else if (!(keyframe_index == keyframe_of(*history, history->current) && layout == state.layout))
  {
    /* Across a topology change the mesh is replaced. */
    result = ApplyResult::Topology;
  }

  StepShape shape = step_shape(*history, index);
  if (state.subdiv_ccg) {
    SubdivCCG &subdiv_ccg = *state.subdiv_ccg;
    subdiv_ccg.positions.as_mutable_span().copy_from(shape.positions);
    if (!subdiv_ccg.masks.is_empty()) {
      subdiv_ccg.masks.as_mutable_span().copy_from(shape.masks);
    }
  }
  else if (result == ApplyResult::Positions) {
    /* Within the current keyframe's span the topology is the mesh's: write positions and masks
     * only, which keeps all other attributes. */
    Mesh &mesh = *id_cast<Mesh *>(ob.data);
    mesh.vert_positions_for_write().copy_from(shape.positions);
    mesh.tag_positions_changed();
    write_mesh_masks(mesh, shape.masks);
  }
  else {
    Mesh *mesh = id_cast<Mesh *>(ob.data);
    Mesh *new_mesh = BKE_mesh_new_nomain(
        keyframe.verts_num, 0, keyframe.faces_num, keyframe.corners_num);
    BKE_mesh_copy_parameters(new_mesh, mesh);
    new_mesh->vert_positions_for_write().copy_from(shape.positions);
    if (keyframe.faces_num > 0) {
      new_mesh->face_offsets_for_write().copy_from({keyframe.face_offsets, keyframe.faces_num + 1});
    }
    new_mesh->corner_verts_for_write().copy_from({keyframe.corner_verts, keyframe.corners_num});
    mesh_calc_edges(*new_mesh, false, false);
    carry_over_attributes(*mesh,
                          *new_mesh,
                          mesh->verts_num == new_mesh->verts_num,
                          mesh_faces_match(*mesh, keyframe));
    write_mesh_masks(*new_mesh, shape.masks);
    BKE_mesh_nomain_to_mesh(new_mesh, mesh, &ob);
  }

  set_runtime(*history, std::move(shape), layout);
  history->current = index;
  return result;
}

bool is_at_current(const Object &ob)
{
  SculptHistory *history = ob.sculpt_history;
  State state;
  if (history == nullptr || history->steps_num == 0 || !read_state(ob, state)) {
    return true;
  }
  const SculptHistoryRuntime &runtime = ensure_runtime(*history);
  if (state.layout != runtime.layout) {
    return false;
  }
  const Span<float3> positions = state.positions;
  if (memcmp(positions.data(), runtime.positions.data(), positions.size_in_bytes()) != 0) {
    return false;
  }
  if (state.masks.is_empty()) {
    return !any_masked(runtime.masks);
  }
  return memcmp(state.masks.data(), runtime.masks.data(), state.masks.size_in_bytes()) == 0;
}

int find_cursor(const SculptHistory &history, const int cursor)
{
  for (const int i : IndexRange(history.steps_num)) {
    if (step_cursor(history.steps[i]) == cursor) {
      return i;
    }
  }
  return -1;
}

int64_t memory_bytes(const SculptHistory &history)
{
  int64_t bytes = sizeof(SculptHistory) + sizeof(SculptHistoryStep) * history.steps_num;
  for (const int i : IndexRange(history.steps_num)) {
    const SculptHistoryStep &step = history.steps[i];
    if (step.positions) {
      bytes += sizeof(float3) * int64_t(step.verts_num) + sizeof(int) * (step.faces_num + 1) +
               sizeof(int) * int64_t(step.corners_num);
    }
    if (step.masks) {
      bytes += sizeof(float) * int64_t(step.verts_num);
    }
    bytes += (sizeof(int) + sizeof(float3)) * int64_t(step.delta_num);
    bytes += (sizeof(int) + sizeof(float)) * int64_t(step.mask_delta_num);
  }
  return bytes;
}

void blend_write(BlendWriter &writer, const SculptHistory &history)
{
  /* Only full file saves carry the steps: undo memfiles don't get the history at all (see
   * #object_undo_preserve, which moves the live history onto the object an undo re-reads). */
  const bool write_steps = !U.sculpt_history_nosave;
  SculptHistory history_copy = history;
  history_copy.runtime = nullptr;
  if (!write_steps) {
    history_copy.steps = nullptr;
    history_copy.steps_num = 0;
    history_copy.current = 0;
  }
  writer.write_struct_at_address(&history, &history_copy);
  if (!write_steps || history.steps_num == 0) {
    return;
  }
  writer.write_struct_array(history.steps_num, history.steps);
  for (const int i : IndexRange(history.steps_num)) {
    const SculptHistoryStep &step = history.steps[i];
    if (step.flag & SCULPT_HISTORY_STEP_KEYFRAME) {
      writer.write_float_array(int64_t(step.verts_num) * 3, step.positions);
      writer.write_int32_array(int64_t(step.faces_num) + 1, step.face_offsets);
      writer.write_int32_array(step.corners_num, step.corner_verts);
      if (step.masks) {
        writer.write_float_array(step.verts_num, step.masks);
      }
    }
    else {
      writer.write_int32_array(step.delta_num, step.delta_indices);
      writer.write_float_array(int64_t(step.delta_num) * 3, step.delta_positions);
      writer.write_int32_array(step.mask_delta_num, step.mask_delta_indices);
      writer.write_float_array(step.mask_delta_num, step.mask_delta_values);
    }
  }
}

/** Reads every array of the step (so all its pointers are valid or null), then checks them. */
static bool read_step(BlendDataReader &reader, SculptHistoryStep &step, const int keyframe_verts)
{
  bool ok = true;
  if (step.flag & SCULPT_HISTORY_STEP_KEYFRAME) {
    ok &= step.verts_num >= 0 && step.faces_num >= 0 && step.corners_num >= 0;
    ok &= BLO_read_array(&reader, &step.positions, int64_t(std::max(step.verts_num, 0)) * 3);
    ok &= BLO_read_array(&reader, &step.face_offsets, int64_t(std::max(step.faces_num, 0)) + 1);
    ok &= BLO_read_array(&reader, &step.corner_verts, std::max(step.corners_num, 0));
    /* Only written when something is masked (and not at all by files from before masks). */
    if (step.masks) {
      ok &= BLO_read_array(&reader, &step.masks, std::max(step.verts_num, 0));
    }
    step.delta_indices = nullptr;
    step.delta_positions = nullptr;
    step.mask_delta_indices = nullptr;
    step.mask_delta_values = nullptr;
    step.mask_delta_num = 0;
    if (!ok || step.positions == nullptr || step.face_offsets == nullptr ||
        (step.corners_num > 0 && step.corner_verts == nullptr))
    {
      return false;
    }
    if (step.grids_num != 0 || step.grid_size != 0) {
      /* Grids: no faces, and as many points as the grids hold. */
      return step.grids_num > 0 && step.grid_size > 1 && step.faces_num == 0 &&
             step.corners_num == 0 &&
             int64_t(step.grids_num) * step.grid_size * step.grid_size == step.verts_num;
    }
    /* Faces must cover the corners in order, and corners must use existing vertices. */
    if (step.face_offsets[0] != 0 || step.face_offsets[step.faces_num] != step.corners_num) {
      return false;
    }
    for (const int i : IndexRange(step.faces_num)) {
      if (step.face_offsets[i + 1] - step.face_offsets[i] < 3) {
        return false;
      }
    }
    for (const int i : IndexRange(step.corners_num)) {
      if (step.corner_verts[i] < 0 || step.corner_verts[i] >= step.verts_num) {
        return false;
      }
    }
    return true;
  }
  ok &= step.delta_num >= 0 && step.mask_delta_num >= 0;
  ok &= BLO_read_array(&reader, &step.delta_indices, std::max(step.delta_num, 0));
  ok &= BLO_read_array(&reader, &step.delta_positions, int64_t(std::max(step.delta_num, 0)) * 3);
  ok &= BLO_read_array(&reader, &step.mask_delta_indices, std::max(step.mask_delta_num, 0));
  ok &= BLO_read_array(&reader, &step.mask_delta_values, std::max(step.mask_delta_num, 0));
  step.positions = nullptr;
  step.face_offsets = nullptr;
  step.corner_verts = nullptr;
  step.masks = nullptr;
  if (!ok || (step.delta_num > 0 && (!step.delta_indices || !step.delta_positions)) ||
      (step.mask_delta_num > 0 && (!step.mask_delta_indices || !step.mask_delta_values)))
  {
    return false;
  }
  for (const int i : IndexRange(step.delta_num)) {
    if (step.delta_indices[i] < 0 || step.delta_indices[i] >= keyframe_verts) {
      return false;
    }
  }
  for (const int i : IndexRange(step.mask_delta_num)) {
    if (step.mask_delta_indices[i] < 0 || step.mask_delta_indices[i] >= keyframe_verts) {
      return false;
    }
  }
  return true;
}

void blend_read(BlendDataReader &reader, Object &ob)
{
  BLO_read_struct(&reader, SculptHistory, &ob.sculpt_history);
  SculptHistory *history = ob.sculpt_history;
  if (history == nullptr) {
    return;
  }
  history->runtime = nullptr;

  bool ok = history->steps_num >= 0;
  if (history->steps_num > 0) {
    ok &= BLO_read_array(&reader, &history->steps, history->steps_num);
    if (history->steps == nullptr) {
      history->steps_num = 0;
      ok = false;
    }
  }
  else {
    history->steps = nullptr;
    history->steps_num = 0;
  }
  /* Read steps are packed: the next appended step grows the storage from here. */
  history->steps_capacity = history->steps_num;

  /* Read all steps even after a failure, so freeing only meets valid pointers. */
  int keyframe_verts = -1;
  for (const int i : IndexRange(history->steps_num)) {
    SculptHistoryStep &step = history->steps[i];
    if (i == 0 && !(step.flag & SCULPT_HISTORY_STEP_KEYFRAME)) {
      /* The first step must be a keyframe; treat it as one to read it safely. */
      step.flag |= SCULPT_HISTORY_STEP_KEYFRAME;
      ok = false;
    }
    if (step.flag & SCULPT_HISTORY_STEP_KEYFRAME) {
      keyframe_verts = step.verts_num;
    }
    ok &= read_step(reader, step, keyframe_verts);
  }
  if (history->steps_num > 0) {
    ok &= history->current >= 0 && history->current < history->steps_num;
  }
  else {
    history->current = 0;
  }

  if (!ok) {
    BLO_reportf_wrap(BLO_read_data_reports(&reader),
                     RPT_WARNING,
                     RPT_("The sculpt history of \"%s\" could not be read and was removed"),
                     ob.id.name + 2);
    free(history);
    ob.sculpt_history = nullptr;
  }
}

}  // namespace blender::bke::sculpt_history
