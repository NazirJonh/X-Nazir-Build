/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Image Editor display of a live 3D (SurfaceAnchored) shape session: every Image Editor showing
 * one of the session's target images draws the shape's contour (the session's UV trace, split at
 * UV seams, drawn through the editor's View2D) and its transform cage (the shared cage gizmo core
 * with a display adapter whose shape-pixel -> region mapping goes through the trace's affine
 * plane-px -> UV Jacobian). An editor with its own live Vector session shows only that one.
 */

#include <cstring>

#include "BLI_listbase_iterator.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "DNA_image_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_context.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "ED_gizmo_library.hh"
#include "ED_paint.hh"
#include "ED_space_api.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../shapes/paint_shape_edit.hh"
#include "../shapes/paint_shape_uv_trace.hh"
#include "../shapes/paint_vector_editor.hh"
#include "paint_image_shape_composite.hh"
#include "paint_shape_gizmo.hh"
#include "paint_shape_vector_3d.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Linked-session lookup
 * \{ */

/** The live 3D session this Image Editor shows, or null: the editor must show one of the
 * session's target images in Paint / View mode, and an editor with its own live Vector session
 * keeps that one (its cage has priority). */
static PaintShapeSession *image3d_linked_session(const bContext *C)
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->image == nullptr) {
    return nullptr;
  }
  if (sima->mode != SI_MODE_PAINT && sima->mode != SI_MODE_VIEW) {
    return nullptr;
  }
  /* UDIM (tiled) images are not supported by the 3D session's UV trace: no contour, no cage. Same
   * criterion as the Python link check. */
  if (sima->image->source == IMA_SRC_TILED) {
    return nullptr;
  }
  if (image_shape_vector_edit_get(sima) != nullptr) {
    return nullptr;
  }
  Object *ob = CTX_data_active_object(C);
  PaintShapeSession *session = ob != nullptr ? paint_shape_session_get(*ob) : nullptr;
  if (session == nullptr || !paint_shape_session_targets_image(*session, *sima->image)) {
    return nullptr;
  }
  return session;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV mapping helpers
 * \{ */

/** The session's current UV trace (rebuilt lazily when the shapes or the surface changed). */
static const ShapeUVTrace *image3d_session_trace(const PaintShapeSession &session,
                                                 const bContext *C)
{
  return paint_shape_session_uv_trace(session, C);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Contour overlay
 * \{ */

static void paint_shape_image3d_region_draw(const bContext *C, ARegion *region, void * /*arg*/)
{
  if (!ED_paint_shape_sessions_alive()) {
    return;
  }
  SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->image == nullptr || region == nullptr) {
    return;
  }
  if (sima->mode != SI_MODE_PAINT && sima->mode != SI_MODE_VIEW) {
    return;
  }
  if (image_shape_vector_edit_get(sima) != nullptr) {
    return;
  }
  Main *bmain = CTX_data_main(C);
  if (bmain == nullptr) {
    return;
  }

  /* By design: the contour draws for every object's linked session shown by this editor, while
   * the cage targets only the active object (the gizmo adapter reads the active object). */
  for (Object &ob : bmain->objects) {
    PaintShapeSession *session = paint_shape_session_get(ob);
    if (session == nullptr || !paint_shape_session_targets_image(*session, *sima->image)) {
      continue;
    }
    const ShapeUVTrace *trace = image3d_session_trace(*session, C);
    if (trace == nullptr) {
      continue;
    }
    for (const int part_i : trace->parts.index_range()) {
      const ShapeUVTracePart &part = trace->parts[part_i];
      Vector<float2> region_points(part.uvs.size());
      for (const int i : part.uvs.index_range()) {
        region_points[i] = image_uv_to_region(*region, part.uvs[i]);
      }
      /* The cage part carries the interactive style; the seam siblings are muted. */
      if (part_i == trace->cage_part) {
        shape_draw_dashed_outline(region_points, part.cyclic);
      }
      else {
        shape_draw_plain_outline(region_points, part.cyclic);
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cage gizmo adapter
 * \{ */

static const PaintShape *image3d_gizmo_target(const bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || image3d_linked_session(C) == nullptr) {
    return nullptr;
  }
  const ShapeEditSession *edit = paint_shape_session_edit(*ob);
  if (edit == nullptr) {
    return nullptr;
  }
  const PaintShape *shape = edit->active();
  if (shape == nullptr || !shape->is_parametric()) {
    return nullptr;
  }
  if (!shape->has_analytic_sdf() && !paint_shape_session_transform_active(*ob)) {
    return nullptr;
  }
  return shape;
}

static const ShapeEditSession *image3d_gizmo_edit(const bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || image3d_linked_session(C) == nullptr) {
    return nullptr;
  }
  return paint_shape_session_edit(*ob);
}

/** The real gesture host of the linked 3D session: the shared 3D host writes every gesture into
 * that session's #ShapeEditSession (same undo, same preview refresh as a 3D gesture). Re-resolved
 * per event, so the host nulls out the moment the session is gone. */
static std::unique_ptr<VectorEditHost> image3d_gizmo_host_create(bContext &C)
{
  PaintShapeSession *session = image3d_linked_session(&C);
  if (session == nullptr) {
    return nullptr;
  }
  return paint_shape_session_host_create(C, *session, 0.0f);
}

/** Region event -> plane pixels: UV (View2D) through the inverse cage Jacobian. The mapping is the
 * session's (frozen for the duration of a drag, see #paint_shape_session_linked_cage_map). */
static bool image3d_gizmo_event_to_shape_px(const bContext *C,
                                            const ARegion &region,
                                            const PaintShape & /*shape*/,
                                            const wmEvent &event,
                                            float2 &r_px)
{
  PaintShapeSession *session = image3d_linked_session(C);
  if (session == nullptr) {
    return false;
  }
  ShapeUVCageMap map;
  if (!paint_shape_session_linked_cage_map(*session, C, map)) {
    return false;
  }
  const float2 uv = image_region_to_uv(region, float2(event.mval[0], event.mval[1]));
  bool ok = false;
  const float2x2 inverse = math::invert(map.jacobian, ok);
  if (!ok) {
    return false;
  }
  r_px = map.ref_px + inverse * (uv - map.ref_uv);
  return true;
}

/** Plane pixels -> region: through the cage Jacobian into UV, then the View2D. */
static float2 image3d_gizmo_shape_px_to_region(const bContext *C,
                                               const ARegion &region,
                                               const PaintShape & /*shape*/,
                                               const float2 &px)
{
  PaintShapeSession *session = image3d_linked_session(C);
  if (session != nullptr) {
    ShapeUVCageMap map;
    if (paint_shape_session_linked_cage_map(*session, C, map)) {
      const float2 uv = map.ref_uv + map.jacobian * (px - map.ref_px);
      return image_uv_to_region(region, uv);
    }
  }
  return px;
}

static const PaintShapeGizmoAdapter g_image3d_gizmo_adapter = {
    /*group_idname=*/"IMAGE_GGT_paint_shape_transform_3d",
    /*poll=*/
    [](const bContext *C) { return image3d_gizmo_target(C) != nullptr; },
    /*target=*/image3d_gizmo_target,
    /*edit=*/image3d_gizmo_edit,
    /*host_create=*/image3d_gizmo_host_create,
    /*event_to_shape_px=*/image3d_gizmo_event_to_shape_px,
    /*shape_px_to_region=*/image3d_gizmo_shape_px_to_region,
    /*is_3d=*/false,
    /*space_matrix=*/nullptr,
};

static bool image3d_shape_transform_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  return paint_shape_gizmo_group_poll(C, g_image3d_gizmo_adapter);
}

static void image3d_shape_transform_gizmo_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  paint_shape_gizmo_group_setup(C, gzgroup, &g_image3d_gizmo_adapter);
}

/** A press on the cage switches this Image Editor's active tool to the shape tool the 3D session
 * was created with, then starts the drag through the shared core. The switch targets the current
 * space (SPACE_IMAGE), so the 3D viewport keeps its own tool, and the session's tool-change
 * observer ignores non-VIEW3D spaces. Best effort: a failed switch leaves the drag working. */
static void image3d_shape_transform_gizmo_invoke_prepare(const bContext *C,
                                                         wmGizmoGroup *gzgroup,
                                                         wmGizmo *gz,
                                                         const wmEvent *event)
{
  PaintShapeSession *session = image3d_linked_session(C);
  /* Capture the cage mapping at the press, before the shared core maps the event and begins the
   * gesture, so a trace rebuild between press and first move cannot make the cage jump. */
  if (session != nullptr) {
    paint_shape_session_freeze_cage_map(*session, C);
  }
  /* Begin the drag (the shared core only needs the gizmo pointers it is handed); the tool switch
   * afterwards cannot disturb the gesture and may refresh the gizmo map. */
  paint_shape_gizmo_group_invoke_prepare(C, gzgroup, gz, event);
  if (session == nullptr) {
    return;
  }
  if (!paint_shape_gizmo_group_drag_active(gzgroup)) {
    /* The press grabbed no handle, so no gesture owns the capture: drop it. */
    paint_shape_session_clear_cage_map(*session);
    return;
  }
  const char *tool_id = paint_shape_session_tool_id(*session);
  if (tool_id == nullptr || tool_id[0] == '\0') {
    return;
  }
  /* Skip the switch (and its screen refresh) when the editor already runs that tool. */
  const bToolRef *current = WM_toolsystem_ref_from_context(C);
  if (current != nullptr && std::strcmp(current->idname, tool_id) == 0) {
    return;
  }
  WM_toolsystem_ref_set_by_id(const_cast<bContext *>(C), tool_id);
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape

namespace blender {

namespace shape = ed::sculpt_paint::shape;

void ED_image_paint_shape3d_draw_register(ARegionType *art)
{
  /* The callback returns early while no 3D session is alive, so it costs nothing otherwise. */
  ED_region_draw_cb_activate(
      art, shape::paint_shape_image3d_region_draw, nullptr, REGION_DRAW_POST_PIXEL);
}

void ED_image_paint_shape3d_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Paint Shape Transform (3D)";
  gzgt->idname = shape::g_image3d_gizmo_adapter.group_idname;

  /* Persistent gesture cage of the linked 3D session: the press starts a drag through the shared
   * core, which writes into the 3D session. */
  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->poll = shape::image3d_shape_transform_gizmo_poll;
  gzgt->setup = shape::image3d_shape_transform_gizmo_setup;
  gzgt->invoke_prepare = shape::image3d_shape_transform_gizmo_invoke_prepare;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = shape::paint_shape_gizmo_group_refresh;
  gzgt->draw_prepare = shape::paint_shape_gizmo_group_draw_prepare;
}

/** True when this Image Editor shows a live 3D Sculpt shape session (its contour and cage). */
bool ED_image_paint_shape3d_session_linked(const bContext *C)
{
  return shape::image3d_linked_session(C) != nullptr;
}

bool ED_image_paint_shape3d_session_apply(bContext *C)
{
  shape::PaintShapeSession *session = shape::image3d_linked_session(C);
  Object *ob = (C != nullptr) ? CTX_data_active_object(C) : nullptr;
  if (session == nullptr || ob == nullptr) {
    return false;
  }
  return shape::paint_shape_session_commit(C, *ob);
}

bool ED_image_paint_shape3d_session_cancel(bContext *C)
{
  shape::PaintShapeSession *session = shape::image3d_linked_session(C);
  Object *ob = (C != nullptr) ? CTX_data_active_object(C) : nullptr;
  if (session == nullptr || ob == nullptr) {
    return false;
  }
  shape::paint_shape_session_cancel(C, *ob);
  return true;
}

}  // namespace blender
