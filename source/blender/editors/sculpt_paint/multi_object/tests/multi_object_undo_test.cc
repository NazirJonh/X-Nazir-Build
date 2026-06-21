/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../multi_object_undo.hh"

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::multi_object::undo::tests {

class MultiObjectUndoTest : public testing::Test {
 public:
  Scene scene;
  Vector<Object *> objects;

  static void SetUpTestSuite()
  {
    BKE_idtype_init();
  }

  void SetUp() override
  {
    for (int i = 0; i < 3; i++) {
      Object *ob = static_cast<Object *>(BKE_id_new_nomain(ID_OB, "TestObject"));
      ob->type = OB_CURVES;
      objects.append(ob);
    }
  }

  void TearDown() override
  {
    for (Object *ob : objects) {
      BKE_id_free(nullptr, ob);
    }
    objects.clear();
  }
};

TEST_F(MultiObjectUndoTest, BasicPushBegin)
{
  Manager manager;

  EXPECT_FALSE(manager.is_active());

  bool result = manager.push_begin(scene, objects, "Test Operation");
  EXPECT_TRUE(result);
  EXPECT_TRUE(manager.is_active());
  EXPECT_EQ(manager.objects().size(), 3);
}

TEST_F(MultiObjectUndoTest, PushBeginTwiceFails)
{
  Manager manager;

  manager.push_begin(scene, objects, "Test 1");
  bool result = manager.push_begin(scene, objects, "Test 2");

  EXPECT_FALSE(result);
}

TEST_F(MultiObjectUndoTest, PushEndWithoutBeginFails)
{
  Manager manager;

  bool result = manager.push_end(objects);
  EXPECT_FALSE(result);
}

TEST_F(MultiObjectUndoTest, FullCycle)
{
  Manager manager;

  EXPECT_TRUE(manager.push_begin(scene, objects, "Test"));
  EXPECT_TRUE(manager.is_active());

  EXPECT_TRUE(manager.push_end(objects));
  EXPECT_FALSE(manager.is_active());
}

TEST_F(MultiObjectUndoTest, Cancel)
{
  Manager manager;

  manager.push_begin(scene, objects, "Test");
  EXPECT_TRUE(manager.is_active());

  manager.cancel();
  EXPECT_FALSE(manager.is_active());
}

TEST_F(MultiObjectUndoTest, MultipleObjects)
{
  Manager manager;

  Vector<Object *> many_objects;
  for (int i = 0; i < 5; i++) {
    Object *ob = static_cast<Object *>(BKE_id_new_nomain(ID_OB, "TestObject"));
    ob->type = OB_CURVES;
    many_objects.append(ob);
  }

  EXPECT_TRUE(manager.push_begin(scene, many_objects, "Multi Test"));
  EXPECT_TRUE(manager.is_active());
  EXPECT_EQ(manager.objects().size(), 5);
  EXPECT_TRUE(manager.push_end(many_objects));
  EXPECT_FALSE(manager.is_active());

  for (Object *ob : many_objects) {
    BKE_id_free(nullptr, ob);
  }
}

TEST_F(MultiObjectUndoTest, EmptyObjectList)
{
  Manager manager;
  Vector<Object *> empty_objects;

  EXPECT_TRUE(manager.push_begin(scene, empty_objects, "Empty Test"));
  EXPECT_TRUE(manager.objects().is_empty());
  EXPECT_TRUE(manager.push_end(empty_objects));
  EXPECT_FALSE(manager.is_active());
}

}  // namespace blender::ed::sculpt_paint::multi_object::undo::tests
