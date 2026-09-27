/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D Viewport Vector shape cage gizmo: the editor-specific adapter over the shared core
 * (`paint_shape_gizmo.cc`). The session works in the SurfaceAnchored space (the tangent plane of
 * the surface under the first press); `is_3d` makes the core place the cage on that plane in world
 * space, so perspective bends it like the shape. Rect / Ellipse always show the cage; a generated
 * Polygon/Star/Arc shows it only while the session's runtime Transform flag is on.
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.hh"

#include "DNA_object_types.h"

#include "BKE_context.hh"

#include "ED_gizmo_library.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_shape.hh"
#include "../paint_shape_edit.hh"
#include "../paint_vector_editor.hh"
#include "paint_shape_gizmo.hh"
#include "paint_shape_vector_3d.hh"

namespace blender::ed::sculpt_paint::shape {

static const PaintShape *view3d_gizmo_target(const bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || !(ob->mode & OB_MODE_SCULPT)) {
    return nullptr;
  }
  const PaintShape *shape = paint_shape_session_active_shape(*ob);
  if (shape == nullptr || !shape->is_parametric()) {
    return nullptr;
  }
  if (!shape->has_analytic_sdf() && !paint_shape_session_transform_active(*ob)) {
    return nullptr;
  }
  return shape;
}

static const ShapeEditSession *view3d_gizmo_edit(const bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob != nullptr ? paint_shape_session_edit(*ob) : nullptr;
}

static std::unique_ptr<VectorEditHost> view3d_gizmo_host_create(bContext &C)
{
  Object *ob = CTX_data_active_object(&C);
  PaintShapeSession *session = ob != nullptr ? paint_shape_session_get(*ob) : nullptr;
  if (session == nullptr) {
    return nullptr;
  }
  return paint_shape_session_host_create(C, *session, 0.0f);
}

/** SurfaceAnchored: region event -> tangent-plane pixels via the view ray ∩ the anchor plane. */
static bool view3d_gizmo_event_to_shape_px(const bContext *C,
                                           const ARegion &region,
                                           const PaintShape & /*shape*/,
                                           const wmEvent &event,
                                           float2 &r_px)
{
  const Object *ob = CTX_data_active_object(C);
  const PaintShapeSession *session = ob != nullptr ? paint_shape_session_get(*ob) : nullptr;
  if (session == nullptr) {
    r_px = float2(event.mval[0], event.mval[1]);
    return true;
  }
  return paint_shape_session_event_to_shape_px(*session, region, event, r_px);
}

static float2 view3d_gizmo_shape_px_to_region(const bContext *C,
                                              const ARegion &region,
                                              const PaintShape & /*shape*/,
                                              const float2 &px)
{
  const Object *ob = CTX_data_active_object(C);
  const PaintShapeSession *session = ob != nullptr ? paint_shape_session_get(*ob) : nullptr;
  return session != nullptr ? paint_shape_session_shape_px_to_region(*session, region, px) : px;
}

/** World matrix of the session's tangent plane; the cage is authored in its pixels (see
 * #paint_shape_session_plane_matrix). */
static bool view3d_gizmo_space_matrix(const bContext *C,
                                      const PaintShape & /*shape*/,
                                      float4x4 &r_space)
{
  const Object *ob = CTX_data_active_object(C);
  const PaintShapeSession *session = ob != nullptr ? paint_shape_session_get(*ob) : nullptr;
  return session != nullptr && paint_shape_session_plane_matrix(*session, r_space);
}

static bool view3d_gizmo_poll(const bContext *C)
{
  return view3d_gizmo_target(C) != nullptr;
}

static const PaintShapeGizmoAdapter g_view3d_gizmo_adapter = {
    /*group_idname=*/"VIEW3D_GGT_paint_shape_transform",
    /*poll=*/view3d_gizmo_poll,
    /*target=*/view3d_gizmo_target,
    /*edit=*/view3d_gizmo_edit,
    /*host_create=*/view3d_gizmo_host_create,
    /*event_to_shape_px=*/view3d_gizmo_event_to_shape_px,
    /*shape_px_to_region=*/view3d_gizmo_shape_px_to_region,
    /*is_3d=*/true,
    /*space_matrix=*/view3d_gizmo_space_matrix,
};

static bool view3d_shape_transform_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  return paint_shape_gizmo_group_poll(C, g_view3d_gizmo_adapter);
}

static void view3d_shape_transform_gizmo_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  paint_shape_gizmo_group_setup(C, gzgroup, &g_view3d_gizmo_adapter);
}

}  // namespace blender::ed::sculpt_paint::shape

namespace blender {

namespace shape = ed::sculpt_paint::shape;

bool ED_view3d_shape_transform_gizmo_hit(const bContext *C, const int mval[2])
{
  return shape::paint_shape_gizmo_hit(C, mval, shape::g_view3d_gizmo_adapter);
}

void ED_view3d_shape_transform_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Paint Shape Transform";
  gzgt->idname = shape::g_view3d_gizmo_adapter.group_idname;

  /* WM_GIZMOGROUPTYPE_3D: `gizmo_window_project_2d` then intersects the view ray with the cage's
   * own plane, which is what makes the 3D cage hit test and its perspective draw work. */
  gzgt->flag =
      WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL | WM_GIZMOGROUPTYPE_3D;

  gzgt->poll = shape::view3d_shape_transform_gizmo_poll;
  gzgt->setup = shape::view3d_shape_transform_gizmo_setup;
  gzgt->invoke_prepare = shape::paint_shape_gizmo_group_invoke_prepare;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = shape::paint_shape_gizmo_group_refresh;
  gzgt->draw_prepare = shape::paint_shape_gizmo_group_draw_prepare;
}

static bool paint_shape_transform_toggle_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob != nullptr && shape::paint_shape_session_get(*ob) != nullptr;
}

static wmOperatorStatus paint_shape_transform_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || shape::paint_shape_session_get(*ob) == nullptr) {
    return OPERATOR_CANCELLED;
  }
  shape::paint_shape_session_transform_toggle(*ob);
  if (ARegion *region = CTX_wm_region(C)) {
    ED_region_tag_redraw(region);
  }
  return OPERATOR_FINISHED;
}

static bool paint_shape_flush_preview_poll(bContext *C)
{
  const Object *ob = CTX_data_active_object(C);
  return ob != nullptr && shape::paint_shape_session_get(*ob) != nullptr;
}

/** Runs from the gizmo refresh callback, i.e. during a redraw, where compositing is unsafe: only
 * arm a modal handler, which composites on the next event (see #paint_shape_flush_preview_modal). */
static wmOperatorStatus paint_shape_flush_preview_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent * /*event*/)
{
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

/** Composite the live preview a gizmo handle drag left stale. A handle drag settles from the
 * gizmo refresh callback, outside any event handler, so nothing else would do it. One-shot: the
 * event is passed on untouched. */
static wmOperatorStatus paint_shape_flush_preview_modal(bContext *C,
                                                        wmOperator * /*op*/,
                                                        const wmEvent * /*event*/)
{
  Object *ob = CTX_data_active_object(C);
  shape::PaintShapeSession *session = ob != nullptr ? shape::paint_shape_session_get(*ob) :
                                                      nullptr;
  if (session != nullptr) {
    std::unique_ptr<shape::VectorEditHost> host = shape::paint_shape_session_host_create(
        *C, *session, 0.0f);
    host->flush_deferred_preview(C);
  }
  return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
}

}  // namespace blender

namespace blender::ed::sculpt_paint {

void SCULPT_OT_paint_shape_flush_preview(wmOperatorType *ot)
{
  ot->name = "Flush Shape Preview";
  ot->idname = "SCULPT_OT_paint_shape_flush_preview";
  ot->description = "Composite the deferred live preview of the Vector shape session";
  ot->invoke = paint_shape_flush_preview_invoke;
  ot->modal = paint_shape_flush_preview_modal;
  ot->poll = paint_shape_flush_preview_poll;
  /* Internal plumbing of the gizmo; never shown or recorded. */
  ot->flag = OPTYPE_INTERNAL;
}

void SCULPT_OT_paint_shape_transform_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Shape Transform";
  ot->idname = "SCULPT_OT_paint_shape_transform_toggle";
  ot->description = "Show the transform cage of the active Polygon/Star/Arc shape";
  ot->exec = paint_shape_transform_toggle_exec;
  ot->poll = paint_shape_transform_toggle_poll;
  /* UI state only; records no undo step. */
  ot->flag = 0;
}

}  // namespace blender::ed::sculpt_paint
