/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared cage gizmo core of the Vector shape session: the cage2d + origin-anchor layout and the
 * handle-to-gesture routing, used by the Image Editor and the 3D Viewport. The editor-specific
 * parts (how to reach the session / host and the shape-pixel <-> region-pixel mapping) live behind
 * #PaintShapeGizmoAdapter; each editor registers its own thin gizmo group around this core.
 */

#pragma once

#include <cstdint>
#include <memory>

#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

namespace blender {
struct ARegion;
struct bContext;
struct wmEvent;
struct wmGizmo;
struct wmGizmoGroup;
namespace ed::sculpt_paint::shape {
struct PaintShape;
class ShapeEditSession;
class VectorEditHost;
}
namespace ed::sculpt_paint::shape {

/** Which session gesture a cage handle drives. */
enum class PaintShapeGizmoHandle : int8_t {
  None = 0,
  Move,
  Rotate,
  /** Corner handle: proportional scale by default, independent X/Y with Shift. */
  ScaleUniform,
  /** Corner handle with Shift: independent X/Y scale. */
  ScaleFree,
  /** Side handle: scale along the shape's local X only. */
  ScaleLocalX,
  /** Side handle: scale along the shape's local Y only. */
  ScaleLocalY,
  Origin,
};

/** Editor-specific hooks of the shared cage gizmo. All pointers are non-null in a real adapter. */
struct PaintShapeGizmoAdapter {
  /** Gizmo group idname; the shared hit test finds the group by it. */
  const char *group_idname = nullptr;
  /** True when the cage should be built / visible in this context. */
  bool (*poll)(const bContext *C) = nullptr;
  /** The active shape the cage edits, or null (the Polygon Transform gate lives here). */
  const PaintShape *(*target)(const bContext *C) = nullptr;
  /** The session's edit session (for `gesture_active`), or null. */
  const ShapeEditSession *(*edit)(const bContext *C) = nullptr;
  /** Build the gesture host of the live session. */
  std::unique_ptr<VectorEditHost> (*host_create)(bContext &C) = nullptr;
  /** Region event -> session (shape) pixels. */
  bool (*event_to_shape_px)(const bContext *C,
                            const ARegion &region,
                            const PaintShape &shape,
                            const wmEvent &event,
                            float2 &r_px) = nullptr;
  /** Session (shape) pixels -> region pixels. */
  float2 (*shape_px_to_region)(const bContext *C,
                              const ARegion &region,
                              const PaintShape &shape,
                              const float2 &px) = nullptr;
  /**
   * True for a 3D viewport cage: #space_matrix places the cage in world space (the anchor's
   * tangent plane, one matrix unit = one shape pixel), so the perspective projection bends the
   * cage exactly like the shape. False keeps the 2D region-pixel affine cage (Image Editor).
   */
  bool is_3d = false;
  /**
   * Required when #is_3d: fill \a r_space with the tangent plane's world matrix (origin = anchor,
   * X/Y = one tangent-plane pixel, Z = plane normal). Returns false when the session is gone or the
   * frame is degenerate; the cage is then skipped this frame.
   */
  bool (*space_matrix)(const bContext *C, const PaintShape &shape, float4x4 &r_space) = nullptr;
};

/** Create the cage + anchor gizmos on \a gzgroup and store \a adapter for the callbacks. */
void paint_shape_gizmo_group_setup(const bContext *C,
                                   wmGizmoGroup *gzgroup,
                                   const PaintShapeGizmoAdapter *adapter);

bool paint_shape_gizmo_group_poll(const bContext *C, const PaintShapeGizmoAdapter &adapter);
void paint_shape_gizmo_group_refresh(const bContext *C, wmGizmoGroup *gzgroup);
void paint_shape_gizmo_group_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup);
void paint_shape_gizmo_group_invoke_prepare(const bContext *C,
                                            wmGizmoGroup *gzgroup,
                                            wmGizmo *gz,
                                            const wmEvent *event);

/** True when \a mval is over the cage / anchor of \a adapter's group in the current region. */
bool paint_shape_gizmo_hit(const bContext *C, const int mval[2], const PaintShapeGizmoAdapter &adapter);

/** True while one of the group's own handles is being dragged (the gesture began). */
bool paint_shape_gizmo_group_drag_active(const wmGizmoGroup *gzgroup);

}  // namespace ed::sculpt_paint::shape
}  // namespace blender
