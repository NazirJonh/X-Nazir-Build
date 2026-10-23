/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 *
 * What the shared overlay needs to draw live control-point editors: the dashed outline of every
 * Vector shape and the shared control-point handles of the active one, or the handles of the
 * modal Bézier input, all in region pixels.
 *
 * The session lives in the editor (a #SpaceImage state or a #PaintShapeSession on an object), so
 * the overlay engine cannot read it directly. These getters publish a snapshot in the space the
 * overlay draws in; the overlay never calls into a session type.
 */

#pragma once

#include <memory>

#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "ED_paint_curve_draw.hh"

namespace blender {
struct ARegion;
struct Object;
struct SpaceImage;
struct SpaceLink;
}  // namespace blender

namespace blender::ed::sculpt_paint::shape_overlay {

/** The live Vector shape session as the shared control-point overlay draws it. */
struct SessionOverlay {
  /** One entry per evaluated outline (a shape may flatten into several parts). */
  blender::Vector<blender::Vector<blender::float2>> outlines;
  blender::Vector<bool> outline_cyclic;
  /** Control points and Bézier handles of the active shape; no solid segments (the dashed
   * outline is the wire). */
  PaintCurveScreenHandles handles;
  bool has_origin = false;
  blender::float2 origin = blender::float2(0.0f);
};

/** Clear \a data, keeping the capacity of its vectors for the next sync. */
inline void session_overlay_clear(SessionOverlay &data)
{
  data.outlines.clear();
  data.outline_cyclic.clear();
  data.handles.clear();
  data.has_origin = false;
}

/**
 * Look of the animated "marching ants" outline of a shape being authored. One definition for the
 * overlay pass, the immediate-mode outline of the session frontends and the modal Bézier input,
 * so the in-progress and the committed contour cannot drift apart.
 */
namespace dash_style {
inline blender::float4 color_a()
{
  return blender::float4(0.4f, 0.4f, 0.4f, 1.0f);
}
inline blender::float4 color_b()
{
  return blender::float4(1.0f, 1.0f, 1.0f, 1.0f);
}
constexpr float WIDTH = 8.0f;
constexpr float UDASH_FACTOR = 0.5f;
}  // namespace dash_style

/**
 * Fill \a r_out from the live Vector session of \a sima, when \a region is the one that owns it.
 * \a cursor is in region pixels and only meaningful while \a cursor_valid; it drives the hover
 * highlight. False when there is no session to draw.
 */
bool image_session_get(const SpaceImage &sima,
                       const ARegion &region,
                       blender::float2 cursor,
                       bool cursor_valid,
                       SessionOverlay &r_out);

/**
 * Same for the live 3D Sculpt Vector session of \a ob. A session is drawn only in the region
 * that owns it: its contour is authored in that view's tangent-plane pixels, so another
 * viewport would project it through the wrong frame entirely.
 */
bool paint_session_get(const Object &ob,
                       const ARegion &region,
                       blender::float2 cursor,
                       bool cursor_valid,
                       SessionOverlay &r_out);

/** What the overlay knows about the region it is syncing; the editors decide which live session
 * (if any) answers it. */
struct SessionQuery {
  const ARegion *region = nullptr;
  /** The space of #region (null when unknown). */
  const SpaceLink *space_data = nullptr;
  const Object *active_object = nullptr;
  bool is_space_image = false;
  bool is_space_v3d = false;
  /** Region pixels; only meaningful while #cursor_valid. */
  blender::float2 cursor = blender::float2(0.0f);
  bool cursor_valid = false;
};

/**
 * The single entry point of the overlay: fill \a r_out from the live control-point session that
 * owns \a query's region, trying the sources in priority order -- the modal Bézier input (the
 * transient creation the user is in the middle of), then the Image Editor Vector session, then
 * the 3D Sculpt Vector session. False when nothing is live there. A new source is added to that
 * one list, not to the overlay.
 */
bool session_overlay_get(const SessionQuery &query, SessionOverlay &r_out);

/**
 * The modal Bézier outline input (Polyline / Curve creation, Select Curve) publishes the shared
 * handles of its placed points here, keyed by the region that owns the input. It is a registry
 * rather than a getter because the input is an operator-local state: the modal pushes a new
 * snapshot whenever it changes, and the overlay reads it without knowing the operator.
 *
 * Only the handles travel through this channel; the dashed outline and the rubber band stay in
 * the operator's region callback.
 */
namespace bezier_overlay {

/**
 * Lifetime token of a published snapshot: the input keeps the only strong reference, the
 * registry a weak one. When the input dies (on any path, not only the ones that remember to call
 * #handles_clear) its snapshot expires with it, so a freed region's address reused by a new
 * region never inherits a stale overlay.
 */
using OwnerToken = std::shared_ptr<bool>;

/** Publish (replacing any previous snapshot) the handles of \a region's active input. */
void handles_set(const ARegion &region,
                 const OwnerToken &owner,
                 const PaintCurveScreenHandles &handles);
/** Drop the snapshot of \a region; the owning operator calls this when it ends. */
void handles_clear(const ARegion &region);
/**
 * The snapshot of \a region, or null when no live input is active there (an expired one is
 * dropped on the way). Valid until the next #handles_set / #handles_clear.
 */
const PaintCurveScreenHandles *handles_find(const ARegion &region);

}  // namespace bezier_overlay

}  // namespace blender::ed::sculpt_paint::shape_overlay
