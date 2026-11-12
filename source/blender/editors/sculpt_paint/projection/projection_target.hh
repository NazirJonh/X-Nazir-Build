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

#include "BLI_math_matrix_types.hh"

#include "DNA_uuid_types.h"

namespace blender {
struct ARegion;
struct bContext;
struct Material;
struct MaterialPaintLayer;
struct Object;
struct View3D;
}  // namespace blender

namespace blender::ed::sculpt_paint::projection {

/** The inline frame of a projected row: centre, full extents, normal and roll. */
struct ProjectionFrame {
  float center[3] = {0.0f, 0.0f, 0.0f};
  float size[3] = {1.0f, 1.0f, 1.0f};
  float normal[3] = {0.0f, 0.0f, 1.0f};
  float roll = 0.0f;
};

/**
 * Transform a normal vector from local to world using transpose(inverse(M3)). One vector at a
 * time: #math::transform_normals is span based (it threads), too heavy for a gesture step.
 */
float3 normal_local_to_world(const float4x4 &o2w, const float3 &local_no);

/** Transform a normal vector from world to a frame object's local space using transpose(M3(o2w)). */
float3 normal_world_to_local(const float4x4 &o2w, const float3 &world_no);

/**
 * The world matrix of \a space_ob -- the object whose space a frame lives in (the controller when
 * one is set, else the painted object) -- as the evaluated depsgraph holds it, so a conversion
 * matches what the bake and the composite read. The un-evaluated matrix is the fallback when no
 * depsgraph is there.
 */
float4x4 projection_space_to_world(const bContext &C, const Object &space_ob);

/** The inverse of #projection_space_to_world; a zero matrix when the space is degenerate, which
 * makes the conversion's own relative-transform check refuse it. */
float4x4 projection_space_from_world(const bContext &C, const Object &space_ob);

/**
 * Raycast the object's evaluated mesh under \a mval: true with the hit's world-space position and
 * normal. The one view raycast the place and move gestures and the decal placement share, so they
 * cannot disagree on what a click lands on.
 */
bool projection_frame_raycast(const bContext &C,
                              const ARegion *region,
                              const View3D *v3d,
                              const Object &ob,
                              const float mval[2],
                              float r_world_loc[3],
                              float r_world_no[3]);

/**
 * Carry a world point and normal into the space of \a space_ob (the controller when one is set,
 * else the painted object). A null \a space_ob, or one without an evaluated copy, copies the world
 * values through.
 */
void projection_frame_point_from_world(const bContext &C,
                                       const Object *space_ob,
                                       const float world_loc[3],
                                       const float world_no[3],
                                       float r_center[3],
                                       float r_normal[3]);

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
