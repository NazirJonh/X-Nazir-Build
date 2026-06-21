/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "testing/testing.h"

#include "../multi_object_undo.hh"
#include "../multi_object_pbvh.hh"

namespace blender::ed::sculpt_paint::multi_object::tests {

class MultiObjectIntegrationTest : public testing::Test {
 protected:
  Scene scene;
  Vector<Object *> objects;
  undo::Manager undo_manager;
  pbvh::Manager pbvh_manager;

  void SetUp() override
  {
    BKE_idtype_init();

    /* Create 3 test objects with Curves */
    for (int i = 0; i < 3; i++) {
      Object *ob = static_cast<Object *>(BKE_id_new_nomain(ID_OB, "TestObject"));
      ob->type = OB_CURVES;

      Curves *curves_id = static_cast<Curves *>(BKE_id_new_nomain(ID_CV, "TestCurves"));
      ob->data = curves_id;

      /* Initialize basic curves geometry */
      bke::CurvesGeometry &curves = curves_id->geometry.wrap();
      curves.resize(5, 15);  /* 5 curves, 15 points total */

      objects.append(ob);
    }
  }

  void TearDown() override
  {
    for (Object *ob : objects) {
      if (ob->data) {
        BKE_id_free(nullptr, ob->data);
      }
      BKE_id_free(nullptr, ob);
    }
    objects.clear();
  }
};

/* Test: Full operation cycle with Undo + PBVH */
TEST_F(MultiObjectIntegrationTest, FullOperationCycle)
{
  /* 1. Initialize PBVH Manager */
  EXPECT_TRUE(pbvh_manager.initialize(objects));
  EXPECT_EQ(pbvh_manager.get_objects().size(), 3);

  /* 2. Begin Undo */
  EXPECT_TRUE(undo_manager.push_begin(scene, objects, "Test Operation"));
  EXPECT_TRUE(undo_manager.is_active());

  /* 3. Find affected nodes */
  float3 brush_pos(0.0f, 0.0f, 0.0f);
  float radius = 1.0f;
  pbvh_manager.find_affected_nodes(brush_pos, radius);

  /* 4. Update bounds */
  pbvh_manager.update_all_bounds();

  /* 5. End Undo */
  EXPECT_TRUE(undo_manager.push_end(objects));
  EXPECT_FALSE(undo_manager.is_active());

  /* 6. Cleanup */
  pbvh_manager.clear();
}

/* Test: Undo + PBVH interaction */
TEST_F(MultiObjectIntegrationTest, UndoPBVHInteraction)
{
  /* Initialize both managers */
  EXPECT_TRUE(pbvh_manager.initialize(objects));
  EXPECT_TRUE(undo_manager.push_begin(scene, objects, "Interaction Test"));

  /* Store original bounds */
  pbvh_manager.store_all_bounds_orig();

  /* Find affected nodes */
  pbvh_manager.find_affected_nodes(float3(0.0f), 1.0f);

  /* Update bounds */
  pbvh_manager.update_all_bounds();

  /* End operation */
  EXPECT_TRUE(undo_manager.push_end(objects));

  /* Verify state */
  EXPECT_FALSE(undo_manager.is_active());
  EXPECT_EQ(pbvh_manager.get_objects().size(), 3);

  pbvh_manager.clear();
}

/* Test: Cancel operation */
TEST_F(MultiObjectIntegrationTest, CancelOperation)
{
  /* Initialize */
  EXPECT_TRUE(pbvh_manager.initialize(objects));
  EXPECT_TRUE(undo_manager.push_begin(scene, objects, "Cancel Test"));

  /* Start operation */
  pbvh_manager.find_affected_nodes(float3(0.0f), 1.0f);

  /* Cancel */
  undo_manager.cancel();
  pbvh_manager.clear();

  /* Verify cleanup */
  EXPECT_FALSE(undo_manager.is_active());
  EXPECT_EQ(pbvh_manager.get_objects().size(), 0);
}

/* Test: Multiple operations in sequence */
TEST_F(MultiObjectIntegrationTest, MultipleOperations)
{
  /* Operation 1 */
  EXPECT_TRUE(pbvh_manager.initialize(objects));
  EXPECT_TRUE(undo_manager.push_begin(scene, objects, "Op1"));
  pbvh_manager.find_affected_nodes(float3(0.0f), 1.0f);
  pbvh_manager.update_all_bounds();
  EXPECT_TRUE(undo_manager.push_end(objects));
  pbvh_manager.clear();

  /* Operation 2 */
  EXPECT_TRUE(pbvh_manager.initialize(objects));
  EXPECT_TRUE(undo_manager.push_begin(scene, objects, "Op2"));
  pbvh_manager.find_affected_nodes(float3(1.0f), 0.5f);
  pbvh_manager.update_all_bounds();
  EXPECT_TRUE(undo_manager.push_end(objects));
  pbvh_manager.clear();

  /* Verify final state */
  EXPECT_FALSE(undo_manager.is_active());
}

/* Test: Empty object list handling */
TEST_F(MultiObjectIntegrationTest, EmptyObjectList)
{
  Vector<Object *> empty_objects;

  /* PBVH should handle empty list. */
  EXPECT_FALSE(pbvh_manager.initialize(empty_objects));
  EXPECT_EQ(pbvh_manager.get_objects().size(), 0);

  /* Undo should handle empty list */
  EXPECT_TRUE(undo_manager.push_begin(scene, empty_objects, "Empty"));
  EXPECT_TRUE(undo_manager.push_end(empty_objects));
  EXPECT_FALSE(undo_manager.is_active());
}

}  // namespace blender::ed::sculpt_paint::multi_object::tests
