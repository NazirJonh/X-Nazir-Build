/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "../multi_object_pbvh.hh"

#include "BKE_curves.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"

#include "testing/testing.h"

namespace blender::ed::sculpt_paint::multi_object::pbvh::tests {

class MultiObjectPBVHTest : public testing::Test {
 public:
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

      Curves *curves_id = static_cast<Curves *>(BKE_id_new_nomain(ID_CV, "TestCurves"));
      ob->data = curves_id;

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

TEST_F(MultiObjectPBVHTest, Initialize)
{
  Manager manager;

  bool result = manager.initialize(objects);
  EXPECT_TRUE(result);

  const Vector<ObjectData> &obj_data = manager.get_objects();
  EXPECT_EQ(obj_data.size(), 3);

  for (int i = 0; i < 3; i++) {
    EXPECT_EQ(obj_data[i].object, objects[i]);
  }
}

TEST_F(MultiObjectPBVHTest, InitializeEmpty)
{
  Manager manager;
  Vector<Object *> empty_objects;

  bool result = manager.initialize(empty_objects);
  EXPECT_FALSE(result);

  const Vector<ObjectData> &obj_data = manager.get_objects();
  EXPECT_EQ(obj_data.size(), 0);
}

TEST_F(MultiObjectPBVHTest, Clear)
{
  Manager manager;

  manager.initialize(objects);
  EXPECT_EQ(manager.get_objects().size(), 3);

  manager.clear();
  EXPECT_EQ(manager.get_objects().size(), 0);
}

TEST_F(MultiObjectPBVHTest, FindAffectedNodes)
{
  Manager manager;
  manager.initialize(objects);

  float3 brush_pos(0.0f, 0.0f, 0.0f);
  float radius = 1.0f;

  manager.find_affected_nodes(brush_pos, radius);

  const Vector<ObjectData> &obj_data = manager.get_objects();
  for (const ObjectData &data : obj_data) {
    EXPECT_TRUE(data.affected_curve_indices.is_empty());
  }
}

TEST_F(MultiObjectPBVHTest, UpdateBounds)
{
  Manager manager;
  manager.initialize(objects);

  manager.update_all_bounds();
}

TEST_F(MultiObjectPBVHTest, StoreBoundsOrig)
{
  Manager manager;
  manager.initialize(objects);

  manager.store_all_bounds_orig();
}

}  // namespace blender::ed::sculpt_paint::multi_object::pbvh::tests
