/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Image Editor Vector shape cage gizmo: the editor-specific adapter over the shared core
 * (`paint_shape_gizmo.cc`). Rect / Ellipse always show the cage; a generated Polygon
 * shows it only while the session's runtime Transform mode is on.
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_vector.hh"

#include "DNA_space_types.h"

#include "BKE_context.hh"

#include "ED_gizmo_library.hh"
#include "ED_paint.hh"

#include "RNA_access.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_image_shape_composite.hh"
#include "paint_shape_gizmo.hh"
#include "../paint_shape_edit.hh"
#include "../paint_vector_editor.hh"

namespace blender::ed::sculpt_paint::shape {

/** The active shape the cage edits, or null. Rect/Ellipse always; Polygon only while the
 * session's runtime Transform mode is on. */
static const PaintShape *image_gizmo_target(const bContext *C)
{
  const SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->mode != SI_MODE_PAINT) {
    return nullptr;
  }
  ShapeEditSession *edit = image_shape_vector_edit_get(sima);
  if (edit == nullptr) {
    return nullptr;
  }
  const PaintShape *shape = edit->active();
  if (shape == nullptr || !shape->is_parametric()) {
    return nullptr;
  }
  if (!shape->has_analytic_sdf() && !image_shape_vector_transform_mode_get(sima)) {
    return nullptr;
  }
  return shape;
}

static const ShapeEditSession *image_gizmo_edit(const bContext *C)
{
  return image_shape_vector_edit_get(CTX_wm_space_image(C));
}

static std::unique_ptr<VectorEditHost> image_gizmo_host_create(bContext &C)
{
  SpaceImage *sima = CTX_wm_space_image(&C);
  ImageShapeVectorState *state = sima != nullptr ? image_shape_vector_state_get(sima) : nullptr;
  if (state == nullptr) {
    return nullptr;
  }
  return image_shape_vector_host_create(C, *state, 0.0f);
}

/** Region event pixel to session (reference-tile) pixels through the editor's view mapping. */
static bool image_gizmo_event_to_shape_px(const bContext *C,
                                          const ARegion &region,
                                          const PaintShape & /*shape*/,
                                          const wmEvent &event,
                                          float2 &r_px)
{
  const float2 uv = image_region_to_uv(region, float2(event.mval[0], event.mval[1]));
  const ShapeEditSession *edit = image_gizmo_edit(C);
  r_px = shape_uv_to_px(edit != nullptr ? edit->canvas_tile() : CanvasTile{}, uv);
  return true;
}

/** Session pixel to region pixel (the inverse of #image_gizmo_event_to_shape_px's map). */
static float2 image_gizmo_shape_px_to_region(const bContext *C,
                                             const ARegion &region,
                                             const PaintShape & /*shape*/,
                                             const float2 &px)
{
  const ShapeEditSession *edit = image_gizmo_edit(C);
  return shape_px_to_region(region, edit != nullptr ? edit->canvas_tile() : CanvasTile{}, px);
}

static bool image_gizmo_poll(const bContext *C)
{
  return image_gizmo_target(C) != nullptr;
}

static const PaintShapeGizmoAdapter g_image_gizmo_adapter = {
    /*group_idname=*/"IMAGE_GGT_paint_shape_transform",
    /*poll=*/image_gizmo_poll,
    /*target=*/image_gizmo_target,
    /*edit=*/image_gizmo_edit,
    /*host_create=*/image_gizmo_host_create,
    /*event_to_shape_px=*/image_gizmo_event_to_shape_px,
    /*shape_px_to_region=*/image_gizmo_shape_px_to_region,
};

static bool paint_shape_transform_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  return paint_shape_gizmo_group_poll(C, g_image_gizmo_adapter);
}

static void paint_shape_transform_gizmo_setup(const bContext *C, wmGizmoGroup *gzgroup)
{
  paint_shape_gizmo_group_setup(C, gzgroup, &g_image_gizmo_adapter);
}

}  // namespace blender::ed::sculpt_paint::shape

namespace blender {

namespace shape = ed::sculpt_paint::shape;

bool ED_image_shape_transform_gizmo_hit(const bContext *C, const int mval[2])
{
  return shape::paint_shape_gizmo_hit(C, mval, shape::g_image_gizmo_adapter);
}

void ED_image_shape_transform_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Paint Shape Transform";
  gzgt->idname = shape::g_image_gizmo_adapter.group_idname;

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->poll = shape::paint_shape_transform_gizmo_poll;
  gzgt->setup = shape::paint_shape_transform_gizmo_setup;
  gzgt->invoke_prepare = shape::paint_shape_gizmo_group_invoke_prepare;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = shape::paint_shape_gizmo_group_refresh;
  gzgt->draw_prepare = shape::paint_shape_gizmo_group_draw_prepare;
}

}  // namespace blender
