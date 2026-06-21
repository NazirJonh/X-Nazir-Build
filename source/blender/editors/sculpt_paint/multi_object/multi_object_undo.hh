/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 * Multi-object undo coordinator for curves sculpt operations.
 */

#include "BLI_map.hh"
#include "BLI_vector.hh"

namespace blender {
struct Object;
struct Scene;
}  // namespace blender

namespace blender::ed::sculpt_paint::multi_object::undo {

/**
 * Coordinator for undo steps across multiple curves sculpt objects.
 *
 * Curves sculpt uses operator-level undo (memfile) rather than mesh PBVH undo.
 * This manager tracks which objects participate in a stroke so future specialized
 * undo steps can be pushed atomically.
 */
class Manager {
 public:
  /**
   * Begin a multi-object sculpt operation.
   */
  bool push_begin(const Scene &scene, const Vector<Object *> &objects, const char *name);

  /**
   * End the multi-object sculpt operation.
   */
  bool push_end(const Vector<Object *> &objects);

  /**
   * Cancel the operation (cleanup on error).
   */
  void cancel();

  /** Check if an operation is active. */
  bool is_active() const { return is_active_; }

  /** Objects registered at the start of the current operation. */
  const Vector<Object *> &objects() const { return objects_; }

  /** Name passed to #push_begin. */
  const char *operation_name() const { return operation_name_; }

 private:
  Vector<Object *> objects_;
  Map<Object *, bool> objects_started_;
  const char *operation_name_ = nullptr;
  bool is_active_ = false;
};

}  // namespace blender::ed::sculpt_paint::multi_object::undo
