/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The editor-facing target of the 3D projection gizmo: the projected row the Layer Material panel
 * shows on the active object's material, read and written through its inline frame values.
 *
 * The row is addressed by its stable marker, so reordering, an undo or a row removal cannot leave
 * the gizmo pointing at freed data. Writes are value-only, which is what makes them safe on every
 * drag step.
 */

#pragma once

#include "DNA_uuid_types.h"

namespace blender {
struct bContext;
struct Material;
struct MaterialPaintLayer;
struct Object;
}  // namespace blender

namespace blender::ed::sculpt_paint::projection {

/** The inline frame of a projected row: centre, full extents, normal and roll. */
struct ProjectionFrame {
  float center[3] = {0.0f, 0.0f, 0.0f};
  float size[3] = {1.0f, 1.0f, 1.0f};
  float normal[3] = {0.0f, 0.0f, 1.0f};
  float roll = 0.0f;
};

class PaintLayerRowProjectionTarget {
 public:
  /**
   * Resolve the shown row of the active object's layered material while it projects a map through
   * its inline frame. False when there is no 3D paint context or the row has no projection.
   */
  bool resolve(const bContext &C);

  /** Read the row's inline frame. False when the row is gone or no longer projects. */
  bool read(ProjectionFrame &r_frame) const;

  /** Write the row's inline frame through the value-only BKE setter. */
  bool write(const ProjectionFrame &frame);

  /** Write the roll, keeping center/size/normal. */
  bool write_roll(float roll);

  Material *material() const
  {
    return material_;
  }

  const bUUID &marker() const
  {
    return marker_;
  }

  /** The painted object, whose space the frame lives in while the row has no controller. */
  Object *object() const
  {
    return object_;
  }

  /**
   * The object whose space the inline frame lives in: the row's controller Empty when it has one,
   * else the painted object. The gizmo draws and drags the frame through this object's transform,
   * so a controlled frame is edited where its stored values are, inside the Empty.
   */
  Object *space_object() const;

  const char *undo_name() const
  {
    return "Edit Projection Frame";
  }

 private:
  MaterialPaintLayer *row() const;

  Material *material_ = nullptr;
  Object *object_ = nullptr;
  bUUID marker_ = {};
};

}  // namespace blender::ed::sculpt_paint::projection
