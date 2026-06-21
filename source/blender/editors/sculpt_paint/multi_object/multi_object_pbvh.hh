/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 * Multi-object PBVH manager for sculpt operations.
 */

#include "BLI_map.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {
struct Object;
}  // namespace blender

namespace blender::ed::sculpt_paint::multi_object::pbvh {

/**
 * Per-object data for multi-object operations.
 */
struct ObjectData {
  Object *object = nullptr;
  Vector<int> affected_curve_indices;
};

/**
 * Manager for PBVH operations across multiple objects.
 */
class Manager {
 public:
  /**
   * Initialize manager with objects.
   */
  bool initialize(Span<Object *> objects);

  /**
   * Find nodes affected by brush in all objects.
   * Uses world-to-local transformation for each object.
   */
  void find_affected_nodes(const float3 &brush_pos_world, float radius);

  /**
   * Update bounds for all affected nodes in all objects.
   */
  void update_all_bounds();

  /**
   * Store original bounds for all affected nodes.
   */
  void store_all_bounds_orig();

  /**
   * Get all object data.
   */
  const Vector<ObjectData> &get_objects() const { return objects_; }

  /**
   * Clear all data.
   */
  void clear();

 private:
  Vector<ObjectData> objects_;
  Map<Object *, int> object_to_index_;
};

}  // namespace blender::ed::sculpt_paint::multi_object::pbvh
