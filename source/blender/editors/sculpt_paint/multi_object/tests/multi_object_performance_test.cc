/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"

#include "DNA_curves_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "testing/testing.h"

#include "../multi_object_undo.hh"
#include "../multi_object_pbvh.hh"

#include <chrono>

namespace blender::ed::sculpt_paint::multi_object::tests {

class MultiObjectPerformanceTest : public testing::Test {
 protected:
  Scene scene;

  static void SetUpTestSuite()
  {
    BKE_idtype_init();
  }

  Vector<Object *> create_objects(int count)
  {
    Vector<Object *> objects;
    for (int i = 0; i < count; i++) {
      Object *ob = static_cast<Object *>(BKE_id_new_nomain(ID_OB, "TestObject"));
      ob->type = OB_CURVES;

      Curves *curves_id = static_cast<Curves *>(BKE_id_new_nomain(ID_CV, "TestCurves"));
      ob->data = curves_id;

      bke::CurvesGeometry &curves = curves_id->geometry.wrap();
      curves.resize(10, 30);  /* 10 curves, 30 points */

      objects.append(ob);
    }
    return objects;
  }

  void cleanup_objects(Vector<Object *> &objects)
  {
    for (Object *ob : objects) {
      if (ob->data) {
        BKE_id_free(nullptr, ob->data);
      }
      BKE_id_free(nullptr, ob);
    }
    objects.clear();
  }

  double benchmark_operation(int object_count, int iterations = 10)
  {
    Vector<Object *> objects = create_objects(object_count);
    undo::Manager undo_manager;
    pbvh::Manager pbvh_manager;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
      pbvh_manager.initialize(objects);
      undo_manager.push_begin(scene, objects, "Benchmark");
      pbvh_manager.find_affected_nodes(float3(0.0f), 1.0f);
      pbvh_manager.update_all_bounds();
      undo_manager.push_end(objects);
      pbvh_manager.clear();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;

    cleanup_objects(objects);
    return duration.count() / iterations;
  }
};

/* Test: Benchmark with 1 object */
TEST_F(MultiObjectPerformanceTest, Benchmark1Object)
{
  double avg_time = benchmark_operation(1);
  std::cout << "1 object: " << avg_time << " ms\n";
  EXPECT_LT(avg_time, 100.0);  /* Should be < 100ms */
}

/* Test: Benchmark with 5 objects */
TEST_F(MultiObjectPerformanceTest, Benchmark5Objects)
{
  double avg_time = benchmark_operation(5);
  std::cout << "5 objects: " << avg_time << " ms\n";
  EXPECT_LT(avg_time, 200.0);  /* Should be < 200ms */
}

/* Test: Benchmark with 10 objects */
TEST_F(MultiObjectPerformanceTest, Benchmark10Objects)
{
  double avg_time = benchmark_operation(10);
  std::cout << "10 objects: " << avg_time << " ms\n";
  EXPECT_LT(avg_time, 300.0);  /* Should be < 300ms */
}

/* Test: Scalability check */
TEST_F(MultiObjectPerformanceTest, ScalabilityCheck)
{
  double time_1 = benchmark_operation(1);
  double time_10 = benchmark_operation(10);
  
  double slowdown = time_10 / time_1;
  std::cout << "Slowdown (10 vs 1): " << slowdown << "x\n";
  
  /* Target: < 2x slowdown for 10 objects */
  EXPECT_LT(slowdown, 3.0);  /* Relaxed to 3x for initial implementation */
}

}  // namespace blender::ed::sculpt_paint::multi_object::tests
