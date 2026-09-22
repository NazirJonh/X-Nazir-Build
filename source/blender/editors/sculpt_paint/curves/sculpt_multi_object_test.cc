/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "sculpt_multi_object.hh"

#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::tests {

/**
 * Builds Curves objects that are not linked into a #Main, which is all the target-selection
 * functions look at: an object type, an object mode and a #Curves pointer.
 */
class CurvesSculptTargetTest : public bke::BlenderGTestBase {
 public:
  Vector<Curves *> curves_ids;
  Vector<std::unique_ptr<Object>> objects;

  void TearDown() override
  {
    objects.clear();
    for (Curves *curves_id : curves_ids) {
      BKE_id_free(nullptr, curves_id);
    }
    curves_ids.clear();
  }

  /** A Curves object in sculpt mode, with its own #Curves data-block. */
  CurvesSculptTarget add_target()
  {
    Curves *curves_id = BKE_id_new_nomain<Curves>("CVtest");
    curves_ids.append(curves_id);

    auto object = std::make_unique<Object>();
    object->type = OB_CURVES;
    object->mode = OB_MODE_SCULPT_CURVES;
    object->data = &curves_id->id;

    Object *object_ptr = object.get();
    objects.append(std::move(object));
    return {object_ptr, curves_id};
  }

  /** A Curves object that is not in sculpt mode. */
  Object &add_object_out_of_mode()
  {
    const CurvesSculptTarget target = this->add_target();
    target.object->mode = OB_MODE_OBJECT;
    return *target.object;
  }
};

TEST_F(CurvesSculptTargetTest, is_target_requires_type_mode_and_data)
{
  const CurvesSculptTarget target = this->add_target();
  EXPECT_TRUE(is_curves_sculpt_target(*target.object));

  target.object->mode = OB_MODE_OBJECT;
  EXPECT_FALSE(is_curves_sculpt_target(*target.object));

  target.object->mode = OB_MODE_SCULPT_CURVES;
  target.object->type = OB_MESH;
  EXPECT_FALSE(is_curves_sculpt_target(*target.object));

  target.object->type = OB_CURVES;
  target.object->data = nullptr;
  EXPECT_FALSE(is_curves_sculpt_target(*target.object));
}

TEST_F(CurvesSculptTargetTest, deform_targets_all_keeps_every_object)
{
  const Vector<CurvesSculptTarget> mode_targets = {
      this->add_target(), this->add_target(), this->add_target()};

  const Vector<CurvesSculptTarget> deform_targets = curves_sculpt_deform_targets(
      mode_targets, CURVES_SCULPT_MULTI_OBJECT_EDIT_ALL);

  ASSERT_EQ(deform_targets.size(), 3);
  for (const int i : mode_targets.index_range()) {
    EXPECT_EQ(deform_targets[i].object, mode_targets[i].object);
  }
}

TEST_F(CurvesSculptTargetTest, deform_targets_active_keeps_only_the_first)
{
  const Vector<CurvesSculptTarget> mode_targets = {this->add_target(), this->add_target()};

  const Vector<CurvesSculptTarget> deform_targets = curves_sculpt_deform_targets(
      mode_targets, CURVES_SCULPT_MULTI_OBJECT_EDIT_ACTIVE);

  ASSERT_EQ(deform_targets.size(), 1);
  EXPECT_EQ(deform_targets[0].object, mode_targets[0].object);
}

TEST_F(CurvesSculptTargetTest, deform_targets_of_nothing_is_nothing)
{
  const Span<CurvesSculptTarget> no_targets;
  EXPECT_TRUE(
      curves_sculpt_deform_targets(no_targets, CURVES_SCULPT_MULTI_OBJECT_EDIT_ACTIVE).is_empty());
  EXPECT_TRUE(
      curves_sculpt_deform_targets(no_targets, CURVES_SCULPT_MULTI_OBJECT_EDIT_ALL).is_empty());
}

TEST_F(CurvesSculptTargetTest, add_targets_follow_the_edit_scope_by_default)
{
  const Vector<CurvesSculptTarget> deform_targets = {this->add_target(), this->add_target()};

  const Vector<CurvesSculptTarget> add_targets = curves_sculpt_add_targets(
      deform_targets, CURVES_SCULPT_ADD_TARGET_ALL, nullptr);

  ASSERT_EQ(add_targets.size(), 2);
  EXPECT_EQ(add_targets[0].object, deform_targets[0].object);
  EXPECT_EQ(add_targets[1].object, deform_targets[1].object);
}

TEST_F(CurvesSculptTargetTest, add_targets_active_keeps_only_the_first)
{
  const Vector<CurvesSculptTarget> deform_targets = {this->add_target(), this->add_target()};

  const Vector<CurvesSculptTarget> add_targets = curves_sculpt_add_targets(
      deform_targets, CURVES_SCULPT_ADD_TARGET_ACTIVE, nullptr);

  ASSERT_EQ(add_targets.size(), 1);
  EXPECT_EQ(add_targets[0].object, deform_targets[0].object);
}

TEST_F(CurvesSculptTargetTest, add_targets_object_overrides_the_edit_scope)
{
  /* The named object is deliberately not among the deform targets: naming an object is a more
   * specific request than the edit scope. */
  const Vector<CurvesSculptTarget> deform_targets = {this->add_target()};
  const CurvesSculptTarget named = this->add_target();

  const Vector<CurvesSculptTarget> add_targets = curves_sculpt_add_targets(
      deform_targets, CURVES_SCULPT_ADD_TARGET_OBJECT, named.object);

  ASSERT_EQ(add_targets.size(), 1);
  EXPECT_EQ(add_targets[0].object, named.object);
  EXPECT_EQ(add_targets[0].curves_id, named.curves_id);
}

TEST_F(CurvesSculptTargetTest, add_targets_active_of_nothing_is_nothing)
{
  const Span<CurvesSculptTarget> no_targets;
  EXPECT_TRUE(
      curves_sculpt_add_targets(no_targets, CURVES_SCULPT_ADD_TARGET_ACTIVE, nullptr).is_empty());
  EXPECT_TRUE(
      curves_sculpt_add_targets(no_targets, CURVES_SCULPT_ADD_TARGET_ALL, nullptr).is_empty());
}

TEST_F(CurvesSculptTargetTest, only_the_first_mode_target_is_active)
{
  CurvesMultiObjectStrokeContext targets;
  targets.mode_targets = {this->add_target(), this->add_target()};
  const CurvesSculptTarget outside = this->add_target();

  EXPECT_TRUE(targets.is_active(*targets.mode_targets[0].object));
  EXPECT_FALSE(targets.is_active(*targets.mode_targets[1].object));
  EXPECT_FALSE(targets.is_active(*outside.object));
}

TEST_F(CurvesSculptTargetTest, nothing_is_active_without_targets)
{
  const CurvesMultiObjectStrokeContext targets;
  const CurvesSculptTarget target = this->add_target();

  EXPECT_FALSE(targets.is_active(*target.object));
}

TEST_F(CurvesSculptTargetTest, target_states_are_kept_per_curves_data_block)
{
  const CurvesSculptTarget first = this->add_target();
  const CurvesSculptTarget second = this->add_target();

  CurvesSculptTargetStates<int> states;
  states.ensure(*first.curves_id) = 1;
  states.ensure(*second.curves_id) = 2;

  EXPECT_EQ(states.ensure(*first.curves_id), 1);
  EXPECT_EQ(states.ensure(*second.curves_id), 2);

  int sum = 0;
  for (const int value : states.values()) {
    sum += value;
  }
  EXPECT_EQ(sum, 3);
}

TEST_F(CurvesSculptTargetTest, add_targets_object_rejects_objects_out_of_sculpt_mode)
{
  const Vector<CurvesSculptTarget> deform_targets = {this->add_target()};
  Object &out_of_mode = this->add_object_out_of_mode();

  EXPECT_TRUE(
      curves_sculpt_add_targets(deform_targets, CURVES_SCULPT_ADD_TARGET_OBJECT, &out_of_mode)
          .is_empty());
  EXPECT_TRUE(curves_sculpt_add_targets(deform_targets, CURVES_SCULPT_ADD_TARGET_OBJECT, nullptr)
                  .is_empty());
}

}  // namespace blender::ed::sculpt_paint::tests
