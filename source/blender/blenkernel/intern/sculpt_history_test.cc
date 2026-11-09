/* SPDX-FileCopyrightText: 2026 Modifications by Nazir
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Unit tests for the sculpt history core. They drive
 * the same entry points the undo pushes do: begin_edit/record/apply_step, on a bare mesh-domain
 * object (no sculpt session, so no Multires grids).
 */

#include "testing/testing.h"

#include "BKE_attribute.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.h"
#include "BKE_object.hh"
#include "BKE_sculpt_history.hh"

#include "BLI_math_vector_types.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"

namespace blender::bke::sculpt_history::tests {

/* The blenkernel fixture runs `BKE_idtype_init` and friends, which ID allocation needs. */
struct sculpt_history : public BlenderGTestBase {};

namespace {

struct Fixture {
  Main *bmain = nullptr;
  Object *ob = nullptr;
  Mesh *mesh = nullptr;

  explicit Fixture(const int verts_num = 4)
  {
    bmain = BKE_main_new();
    mesh = BKE_mesh_new_nomain(verts_num, 0, 0, 0);
    ob = BKE_object_add_only_object(bmain, OB_MESH, "O");
    ob->data = &mesh->id;
  }

  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;

  ~Fixture()
  {
    if (mesh != nullptr) {
      BKE_id_free(nullptr, &mesh->id);
    }
    BKE_main_free(bmain);
  }
};

void set_positions(Mesh &mesh, const std::initializer_list<float3> &coords)
{
  MutableSpan<float3> positions = mesh.vert_positions_for_write();
  int i = 0;
  for (const float3 &c : coords) {
    positions[i++] = c;
  }
}

Span<float3> read_positions(const Object &ob)
{
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  return mesh.vert_positions();
}

void set_mask(Mesh &mesh, const int index, const float value)
{
  MutableAttributeAccessor attributes = mesh.attributes_for_write();
  /* Zero-initialized: the write-only variant leaves the other points' values unset. */
  SpanAttributeWriter<float> writer = attributes.lookup_or_add_for_write_span<float>(
      ".sculpt_mask", AttrDomain::Point);
  writer.span[index] = value;
  writer.finish();
}

}  // namespace

TEST_F(sculpt_history, ensure_creates_recording_history)
{
  Fixture f;
  SculptHistory &history = ensure(*f.ob);
  EXPECT_EQ(history.steps_num, 0);
  EXPECT_EQ(history.current, 0);
}

TEST_F(sculpt_history, begin_edit_makes_keyframe)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_NE(history, nullptr);
  ASSERT_EQ(history->steps_num, 1);
  EXPECT_NE(history->steps[0].flag & SCULPT_HISTORY_STEP_KEYFRAME, 0);

  /* Calling record without changes adds no step. */
  record(*f.ob, "Draw", false);
  EXPECT_EQ(history->steps_num, 1);
}

TEST_F(sculpt_history, delta_record_and_apply_roundtrip_bit_exact)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");

  f.mesh->vert_positions_for_write()[1] = float3(2.0f, 0.0f, 0.0f);
  record(*f.ob, "Move", false);

  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 2);
  const SculptHistoryStep &step = history->steps[1];
  EXPECT_EQ(step.delta_num, 1);
  EXPECT_EQ(step.delta_indices[0], 1);

  /* Back to the keyframe: the moved vertex gets its exact original value. */
  EXPECT_EQ(apply_step(*f.ob, 0), ApplyResult::Positions);
  EXPECT_EQ(read_positions(*f.ob)[1], (float3{1.0f, 0.0f, 0.0f}));
  /* The mesh now holds the current step (0) bit-exactly. */
  EXPECT_TRUE(is_at_current(*f.ob));
  /* Forward again, exactly. */
  EXPECT_EQ(apply_step(*f.ob, 1), ApplyResult::Positions);
  EXPECT_EQ(read_positions(*f.ob)[1], (float3{2.0f, 0.0f, 0.0f}));
  EXPECT_TRUE(is_at_current(*f.ob));
}

TEST_F(sculpt_history, empty_edit_adds_no_step)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  const int steps_before = f.ob->sculpt_history->steps_num;
  record(*f.ob, "Nothing", false);
  EXPECT_EQ(f.ob->sculpt_history->steps_num, steps_before);
}

TEST_F(sculpt_history, mask_delta_record_and_apply)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");

  set_mask(*f.mesh, 2, 0.5f);
  record(*f.ob, "Mask", false);
  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 2);
  EXPECT_EQ(history->steps[1].mask_delta_num, 1);
  EXPECT_EQ(history->steps[1].mask_delta_indices[0], 2);
  EXPECT_TRUE(step_has_mask(*history, 1));

  /* Scrubbing back clears the mask (the attribute may be removed when nothing is masked). */
  EXPECT_EQ(apply_step(*f.ob, 0), ApplyResult::Positions);
  const AttributeReader<float> masks =
      f.mesh->attributes().lookup<float>(".sculpt_mask", AttrDomain::Point);
  if (masks) {
    EXPECT_EQ(masks.varray[2], 0.0f);
  }
  EXPECT_FALSE(step_has_mask(*history, 0));
  EXPECT_EQ(apply_step(*f.ob, 1), ApplyResult::Positions);
  const AttributeReader<float> masks_again =
      f.mesh->attributes().lookup<float>(".sculpt_mask", AttrDomain::Point);
  ASSERT_TRUE(bool(masks_again));
  EXPECT_EQ(masks_again.varray[2], 0.5f);
}

TEST_F(sculpt_history, truncate_after_current_drops_future)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  f.mesh->vert_positions_for_write()[1] = float3(2, 0, 0);
  record(*f.ob, "Move 1", false);
  f.mesh->vert_positions_for_write()[1] = float3(3, 0, 0);
  record(*f.ob, "Move 2", false);
  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 3);

  apply_step(*f.ob, 1);
  truncate_after_current(*history);
  EXPECT_EQ(history->steps_num, 2);
  EXPECT_EQ(history->current, 1);

  /* The next edit takes the place of the dropped steps. */
  f.mesh->vert_positions_for_write()[2] = float3(0, 2, 0);
  record(*f.ob, "Other branch", false);
  EXPECT_EQ(history->steps_num, 3);
  EXPECT_EQ(history->current, 2);
  apply_step(*f.ob, 0);
  EXPECT_EQ(read_positions(*f.ob)[1], (float3{1.0f, 0.0f, 0.0f}));
}

TEST_F(sculpt_history, apply_limit_folds_into_the_first_keyframe)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  for (int i = 1; i <= 5; i++) {
    f.mesh->vert_positions_for_write()[1] = float3(1.0f + i, 0, 0);
    record(*f.ob, "Step", false);
  }
  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 6);

  U.sculpt_history_limit = 3;
  apply_limit(*history);
  U.sculpt_history_limit = 0;

  EXPECT_EQ(history->steps_num, 3);
  /* The keyframe carries the folded shape: the state of the last dropped step (x = 4). The
   * current step is untouched, so the last index is read before scrubbing moves it. */
  const int last = history->current;
  apply_step(*f.ob, 0);
  EXPECT_EQ(read_positions(*f.ob)[1].x, 4.0f);
  apply_step(*f.ob, last);
  EXPECT_EQ(read_positions(*f.ob)[1].x, 6.0f);
}

TEST_F(sculpt_history, apply_limit_keeps_the_current_step_at_the_start)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  for (int i = 1; i <= 5; i++) {
    f.mesh->vert_positions_for_write()[1] = float3(1.0f + i, 0, 0);
    record(*f.ob, "Step", false);
  }
  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 6);
  apply_step(*f.ob, 0);
  ASSERT_EQ(history->current, 0);

  U.sculpt_history_limit = 3;
  apply_limit(*history);
  U.sculpt_history_limit = 0;

  /* Nothing can be dropped without dropping the current step. */
  EXPECT_EQ(history->steps_num, 6);
  EXPECT_EQ(history->current, 0);
}

TEST_F(sculpt_history, record_compares_only_the_candidate_points)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");

  f.mesh->vert_positions_for_write()[1] = float3(2, 0, 0);
  f.mesh->vert_positions_for_write()[3] = float3(2, 2, 0);
  /* The undo step touched only point 1 (each point once: the callers pass sorted unique ones). */
  record(*f.ob, "Move", false, {1});

  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 2);
  ASSERT_EQ(history->steps[1].delta_num, 1);
  EXPECT_EQ(history->steps[1].delta_indices[0], 1);
}

TEST_F(sculpt_history, topology_record_makes_keyframe_and_apply_replaces_mesh)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");

  /* Pretend a remesh: another point count forces a keyframe through the layout check. */
  Mesh *current = BKE_mesh_new_nomain(3, 0, 0, 0);
  set_positions(*current, {{5, 5, 5}, {6, 5, 5}, {5, 6, 5}});
  f.ob->data = &current->id;
  record(*f.ob, "Remesh", false);
  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 2);
  EXPECT_NE(history->steps[1].flag & SCULPT_HISTORY_STEP_KEYFRAME, 0);

  /* Back to the old shape: the mesh is replaced in place, the points come bit-exactly. */
  EXPECT_EQ(apply_step(*f.ob, 0), ApplyResult::Topology);
  EXPECT_EQ(read_positions(*f.ob).size(), 4);
  EXPECT_EQ(read_positions(*f.ob)[3], (float3{1.0f, 1.0f, 0.0f}));
  EXPECT_EQ(id_cast<Mesh *>(f.ob->data), current);

  /* `current` now holds the old shape and is no longer referenced by the object's history;
   * the fixture frees the original `f.mesh`, which the object dropped when it was replaced. */
  BKE_id_free(nullptr, &current->id);
}

TEST_F(sculpt_history, cursor_get_set_and_find)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  f.mesh->vert_positions_for_write()[1] = float3(2, 0, 0);
  record(*f.ob, "Move", false);

  SculptHistory *history = f.ob->sculpt_history;
  const int cursor = cursor_get(*f.ob);
  EXPECT_EQ(cursor, history->steps[history->current].number + 1);

  apply_step(*f.ob, 0);
  cursor_set(*f.ob, cursor);
  EXPECT_EQ(history->current, 1);
  EXPECT_EQ(find_cursor(*history, cursor), 1);
  EXPECT_EQ(find_cursor(*history, 9999), -1);
}

TEST_F(sculpt_history, recording_off_adds_nothing)
{
  Fixture f;
  U.sculpt_history_off = 1;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  f.mesh->vert_positions_for_write()[1] = float3(2, 0, 0);
  record(*f.ob, "Move", false);
  U.sculpt_history_off = 0;
  EXPECT_EQ(f.ob->sculpt_history, nullptr);
}

TEST_F(sculpt_history, scoped_pause_blocks_recording)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  {
    ScopedPause pause(*f.ob);
    f.mesh->vert_positions_for_write()[1] = float3(2, 0, 0);
    record(*f.ob, "Hidden", false);
  }
  EXPECT_EQ(f.ob->sculpt_history->steps_num, 1);
}

TEST_F(sculpt_history, remove_and_free)
{
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  ASSERT_NE(f.ob->sculpt_history, nullptr);
  const int64_t bytes = memory_bytes(*f.ob->sculpt_history);
  EXPECT_GT(bytes, 0);
  remove(*f.ob);
  EXPECT_EQ(f.ob->sculpt_history, nullptr);
}

TEST_F(sculpt_history, step_shape_backward_from_runtime)
{
  /* Regression test for the Back bug: step_shape with index < current must rebuild from the
   * keyframe, not use the runtime shortcut (which only works forward). */
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  /* Step 1: move vertex 1. */
  f.mesh->vert_positions_for_write()[1] = float3(2.0f, 0.0f, 0.0f);
  record(*f.ob, "Move 1", false);
  /* Step 2: move vertex 2. */
  f.mesh->vert_positions_for_write()[2] = float3(0.0f, 2.0f, 0.0f);
  record(*f.ob, "Move 2", false);

  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 3);
  ASSERT_EQ(history->current, 2);

  /* Runtime is populated for step 2. */
  ASSERT_NE(history->runtime, nullptr);

  /* step_shape(2) should return step 2's shape (current runtime is valid). */
  StepShape shape2 = step_shape(*history, 2);
  EXPECT_EQ(shape2.positions[1], (float3{2.0f, 0.0f, 0.0f}));
  EXPECT_EQ(shape2.positions[2], (float3{0.0f, 2.0f, 0.0f}));

  /* step_shape(0) should return the keyframe shape, not step 2's shape. */
  StepShape shape0 = step_shape(*history, 0);
  EXPECT_EQ(shape0.positions[1], (float3{1.0f, 0.0f, 0.0f}));
  EXPECT_EQ(shape0.positions[2], (float3{0.0f, 1.0f, 0.0f}));

  /* step_shape(1) should return step 1's shape. */
  StepShape shape1 = step_shape(*history, 1);
  EXPECT_EQ(shape1.positions[1], (float3{2.0f, 0.0f, 0.0f}));
  EXPECT_EQ(shape1.positions[2], (float3{0.0f, 1.0f, 0.0f}));
}

TEST_F(sculpt_history, apply_step_backward_forward_roundtrip)
{
  /* Full integration test: apply_step backward then forward should restore exact state. */
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  set_mask(*f.mesh, 3, 0.75f);
  begin_edit(*f.ob, "Start");
  /* Step 1: move vertex 1. */
  f.mesh->vert_positions_for_write()[1] = float3(2.0f, 0.0f, 0.0f);
  record(*f.ob, "Move 1", false);
  /* Step 2: move vertex 2 and change mask. */
  f.mesh->vert_positions_for_write()[2] = float3(0.0f, 2.0f, 0.0f);
  set_mask(*f.mesh, 3, 0.25f);
  record(*f.ob, "Move 2", false);

  const float3 final_pos1 = read_positions(*f.ob)[1];
  const float3 final_pos2 = read_positions(*f.ob)[2];
  const AttributeReader<float> final_masks =
      f.mesh->attributes().lookup<float>(".sculpt_mask", AttrDomain::Point);
  const float final_mask3 = final_masks.varray[3];

  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->current, 2);

  /* Go backward to step 0. */
  EXPECT_EQ(apply_step(*f.ob, 0), ApplyResult::Positions);
  EXPECT_EQ(read_positions(*f.ob)[1], (float3{1.0f, 0.0f, 0.0f}));
  EXPECT_EQ(read_positions(*f.ob)[2], (float3{0.0f, 1.0f, 0.0f}));
  EXPECT_TRUE(is_at_current(*f.ob));

  /* Go forward to step 2. */
  EXPECT_EQ(apply_step(*f.ob, 2), ApplyResult::Positions);
  EXPECT_EQ(read_positions(*f.ob)[1], final_pos1);
  EXPECT_EQ(read_positions(*f.ob)[2], final_pos2);
  const AttributeReader<float> restored_masks =
      f.mesh->attributes().lookup<float>(".sculpt_mask", AttrDomain::Point);
  EXPECT_EQ(restored_masks.varray[3], final_mask3);
  EXPECT_TRUE(is_at_current(*f.ob));
}

TEST_F(sculpt_history, back_then_new_stroke_truncates_correctly)
{
  /* Regression test: after Back (apply_step backward), a new stroke should truncate only the
   * truly future steps, not corrupt the runtime. */
  Fixture f;
  set_positions(*f.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  begin_edit(*f.ob, "Start");
  f.mesh->vert_positions_for_write()[1] = float3(2.0f, 0.0f, 0.0f);
  record(*f.ob, "Move 1", false);
  f.mesh->vert_positions_for_write()[2] = float3(0.0f, 2.0f, 0.0f);
  record(*f.ob, "Move 2", false);
  f.mesh->vert_positions_for_write()[3] = float3(1.0f, 2.0f, 0.0f);
  record(*f.ob, "Move 3", false);

  SculptHistory *history = f.ob->sculpt_history;
  ASSERT_EQ(history->steps_num, 4);
  ASSERT_EQ(history->current, 3);

  /* Go back to step 1. */
  EXPECT_EQ(apply_step(*f.ob, 1), ApplyResult::Positions);
  EXPECT_EQ(history->current, 1);
  EXPECT_EQ(read_positions(*f.ob)[1], (float3{2.0f, 0.0f, 0.0f}));
  EXPECT_EQ(read_positions(*f.ob)[2], (float3{0.0f, 1.0f, 0.0f}));  // Step 2's change undone.
  EXPECT_TRUE(is_at_current(*f.ob));

  /* New stroke: truncate steps 2 and 3, add a new step. */
  f.mesh->vert_positions_for_write()[0] = float3(5.0f, 5.0f, 5.0f);
  record(*f.ob, "New branch", false);
  EXPECT_EQ(history->steps_num, 3);  // Keyframe, step 1, new step.
  EXPECT_EQ(history->current, 2);

  /* Verify runtime matches the mesh. */
  EXPECT_TRUE(is_at_current(*f.ob));
  EXPECT_EQ(read_positions(*f.ob)[0], (float3{5.0f, 5.0f, 5.0f}));
  EXPECT_EQ(read_positions(*f.ob)[1], (float3{2.0f, 0.0f, 0.0f}));

  /* The new delta should be correct. */
  const SculptHistoryStep &new_step = history->steps[2];
  EXPECT_EQ(new_step.delta_num, 1);
  EXPECT_EQ(new_step.delta_indices[0], 0);
}

TEST_F(sculpt_history, two_objects_have_independent_histories)
{
  /* Two objects with distinct meshes have completely independent histories: steps, cursors, and
   * runtime are per-object. Actions on one object (apply_step, cursor_set, record) do not affect
   * the other's history or mesh. */
  Fixture f_a(4);
  Fixture f_b(4);
  set_positions(*f_a.mesh, {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}});
  set_positions(*f_b.mesh, {{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {2, 2, 0}});

  begin_edit(*f_a.ob, "Start A");
  begin_edit(*f_b.ob, "Start B");

  /* Object A: two edits. */
  f_a.mesh->vert_positions_for_write()[1] = float3(5.0f, 0.0f, 0.0f);
  record(*f_a.ob, "A1", false);
  f_a.mesh->vert_positions_for_write()[2] = float3(0.0f, 5.0f, 0.0f);
  record(*f_a.ob, "A2", false);

  /* Object B: one edit. */
  f_b.mesh->vert_positions_for_write()[3] = float3(9.0f, 9.0f, 0.0f);
  record(*f_b.ob, "B1", false);

  SculptHistory *history_a = f_a.ob->sculpt_history;
  SculptHistory *history_b = f_b.ob->sculpt_history;
  ASSERT_NE(history_a, nullptr);
  ASSERT_NE(history_b, nullptr);

  /* Independent step counts. */
  EXPECT_EQ(history_a->steps_num, 3);
  EXPECT_EQ(history_b->steps_num, 2);
  EXPECT_EQ(history_a->current, 2);
  EXPECT_EQ(history_b->current, 1);

  /* Independent cursors: distinct step numbers. */
  const int cursor_a = cursor_get(*f_a.ob);
  const int cursor_b = cursor_get(*f_b.ob);
  EXPECT_NE(cursor_a, NO_CURSOR);
  EXPECT_NE(cursor_b, NO_CURSOR);
  EXPECT_NE(cursor_a, cursor_b);

  /* apply_step on A does not affect B's mesh or history. */
  const float3 b_vert3_before = read_positions(*f_b.ob)[3];
  EXPECT_EQ(b_vert3_before, (float3{9.0f, 9.0f, 0.0f}));

  EXPECT_EQ(apply_step(*f_a.ob, 0), ApplyResult::Positions);
  EXPECT_EQ(read_positions(*f_a.ob)[1], (float3{1.0f, 0.0f, 0.0f}));  // Back to keyframe.
  EXPECT_EQ(read_positions(*f_a.ob)[2], (float3{0.0f, 1.0f, 0.0f}));
  EXPECT_TRUE(is_at_current(*f_a.ob));
  EXPECT_EQ(history_a->current, 0);

  /* B unchanged: mesh and history state. */
  EXPECT_EQ(read_positions(*f_b.ob)[3], b_vert3_before);
  EXPECT_EQ(history_b->current, 1);
  EXPECT_TRUE(is_at_current(*f_b.ob));

  /* cursor_set on B does not affect A's history or mesh. */
  const int cursor_b_step0 = step_cursor(history_b->steps[0]);
  cursor_set(*f_b.ob, cursor_b_step0);
  EXPECT_EQ(history_b->current, 0);
  /* cursor_set moves the cursor but does not modify the mesh: B's mesh still holds step 1. */
  EXPECT_EQ(read_positions(*f_b.ob)[3], b_vert3_before);
  EXPECT_FALSE(is_at_current(*f_b.ob));

  /* A's history unaffected by cursor_set on B. */
  EXPECT_EQ(history_a->current, 0);
  EXPECT_EQ(read_positions(*f_a.ob)[1], (float3{1.0f, 0.0f, 0.0f}));

  /* Mimic editors' history_sync for both objects: apply_step to match cursor when out of sync. */
  if (!is_at_current(*f_a.ob)) {
    const int target_a = find_cursor(*history_a, cursor_get(*f_a.ob));
    EXPECT_GE(target_a, 0);
    apply_step(*f_a.ob, target_a);
  }
  if (!is_at_current(*f_b.ob)) {
    const int target_b = find_cursor(*history_b, cursor_get(*f_b.ob));
    EXPECT_GE(target_b, 0);
    apply_step(*f_b.ob, target_b);
  }
  EXPECT_TRUE(is_at_current(*f_a.ob));
  EXPECT_TRUE(is_at_current(*f_b.ob));

  /* Both now at step 0: meshes match their keyframes. */
  EXPECT_EQ(history_a->current, 0);
  EXPECT_EQ(history_b->current, 0);
  EXPECT_EQ(read_positions(*f_a.ob)[1], (float3{1.0f, 0.0f, 0.0f}));
  EXPECT_EQ(read_positions(*f_a.ob)[2], (float3{0.0f, 1.0f, 0.0f}));
  EXPECT_EQ(read_positions(*f_b.ob)[3], (float3{2.0f, 2.0f, 0.0f}));

  /* New record on A after stepping back: truncates A's future, B unaffected. */
  const int steps_b_before = history_b->steps_num;
  const int current_b_before = history_b->current;

  f_a.mesh->vert_positions_for_write()[0] = float3(7.0f, 7.0f, 7.0f);
  record(*f_a.ob, "A new branch", false);
  EXPECT_EQ(history_a->steps_num, 2);  // Keyframe + new step (steps 1 and 2 dropped).
  EXPECT_EQ(history_a->current, 1);

  /* B completely unaffected. */
  EXPECT_EQ(history_b->steps_num, steps_b_before);
  EXPECT_EQ(history_b->current, current_b_before);
  EXPECT_EQ(read_positions(*f_b.ob)[3], (float3{2.0f, 2.0f, 0.0f}));
}

}  // namespace blender::bke::sculpt_history::tests
