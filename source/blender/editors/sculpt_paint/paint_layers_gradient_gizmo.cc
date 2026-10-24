/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The persistent handles of the active Gradient row. The row stores its line in its own space
 * (UV, object or generated); this gizmo projects that line to the editor and writes the moved
 * handle back through the same BKE setters the Gradient panel uses, so the row always stays the
 * single source of truth and the handles reappear where the row says after a tool, row or file
 * change.
 *
 * The line and discs are the Color Gradient tool's (see #gradient_gizmo), so both tools read as
 * one; only the ownership differs: the tool edits a temporary gesture, this edits the layer.
 */

#include <cfloat>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_gradient_surface.hh"

#include "ED_gizmo_library.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "IMB_colormanagement.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_gradient_gizmo.hh"
#include "paint_layers_gradient.hh"

namespace blender {

namespace {

/** The shared line/handle drawing and picking lives beside this file's paint operators. */
using namespace ed::sculpt_paint;

/** The row whose handles are shown, plus the data the projection reads. */
struct LayerGradientTarget {
  Material *material = nullptr;
  MaterialPaintLayer *layer = nullptr;
  Object *object = nullptr;
  const Mesh *mesh = nullptr;

  const MaterialPaintLayerGradient *gradient() const
  {
    return (layer != nullptr) ? layer->gradient : nullptr;
  }

  bool valid() const
  {
    return material != nullptr && layer != nullptr && gradient() != nullptr;
  }
};

/**
 * Resolve the active row. \a need_mesh is false in the Image Editor, where the line is UV-only and
 * no surface is sampled.
 */
LayerGradientTarget layer_gradient_target_resolve(const bContext &C, const bool need_mesh)
{
  LayerGradientTarget target;
  target.object = CTX_data_active_object(&C);
  if (target.object == nullptr) {
    return target;
  }
  target.material = paint_layer_gradient_target(&C, &target.layer);
  if (target.material == nullptr || target.layer == nullptr) {
    return target;
  }
  if (need_mesh) {
    if (target.object->type != OB_MESH) {
      return LayerGradientTarget();
    }
    target.mesh = paint_layer_gradient_mesh_eval(C, *target.object);
    if (target.mesh == nullptr) {
      return LayerGradientTarget();
    }
  }
  return target;
}

struct LayerGradientGizmoGroup {
  wmGizmo *gizmo = nullptr;
  /** The Image Editor maps UV to screen pixels; the 3D View projects world anchors. */
  bool image_editor = false;

  bool points_valid = false;
  float2 start_px = float2(0.0f);
  float2 end_px = float2(0.0f);
  float midpoint = 0.5f;

  /** The handle the running drag moves, or #gradient_gizmo::NONE. */
  int drag_handle = gradient_gizmo::NONE;
  float3 start_before = float3(0.0f);
  float3 end_before = float3(0.0f);
  float midpoint_before = 0.5f;
  /** The surface point of the press, for a drag whose end misses the mesh. */
  PaintLayerGradientHit plane_hit;
  bool has_plane_hit = false;
  /** The world point a free (non-snapping) handle drag moves in the view plane: the grabbed
   * handle's position when the drag started. */
  float3 drag_plane_point = float3(0.0f);
  bool has_drag_plane = false;

  /** The shapes' colors and the direction arrow, refilled from the row's ramp on every update. */
  gradient_gizmo::DrawOptions draw_options;

  /** The row the drag started on; a row or material switch cancels the drag. */
  Material *session_material = nullptr;
  bUUID session_marker = {};

  /** UV-to-surface triangle cache per end, reset when the object, mesh, row or space changes. */
  Material *cache_material = nullptr;
  bUUID cache_marker = {};
  int8_t cache_space = -1;
  Object *cache_object = nullptr;
  const Mesh *cache_mesh = nullptr;
  int start_tri = -1;
  int end_tri = -1;
  /** The row points #start_tri / #end_tri belong to, and whether they were found. A point known
   * to be outside the UV islands is not searched again until the line changes: a full triangle
   * scan on every redraw would be the gizmo's whole cost on a dense mesh. */
  float3 start_point_cache = float3(FLT_MAX);
  bool start_found = false;
  float3 end_point_cache = float3(FLT_MAX);
  bool end_found = false;
};

LayerGradientGizmoGroup *group_data(wmGizmoGroup *gzgroup)
{
  return static_cast<LayerGradientGizmoGroup *>(gzgroup->customdata);
}

LayerGradientGizmoGroup *group_data(const wmGizmo *gz)
{
  return group_data(gz->parent_gzgroup);
}

bool target_is_session(const LayerGradientTarget &target, const LayerGradientGizmoGroup &ggd)
{
  return target.valid() && target.material == ggd.session_material &&
         BLI_uuid_equal(target.layer->marker, ggd.session_marker);
}

void notify_changed(bContext &C, Material &material)
{
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
  if (ARegion *region = CTX_wm_region(&C)) {
    ED_region_tag_redraw(region);
  }
}

/** Whether a dragged handle is placed on the surface instead of moving freely in the view plane.
 */
bool snap_enabled(const bContext &C)
{
  const Scene *scene = CTX_data_scene(&C);
  return scene != nullptr && scene->toolsettings != nullptr &&
         scene->toolsettings->paint_mode.gradient_snap != 0;
}

/* -------------------------------------------------------------------- */
/** \name Screen points
 * \{ */

/** Fill the group's screen points from the row; false when the line cannot be placed. */
bool points_update(const bContext &C, LayerGradientGizmoGroup &ggd)
{
  ARegion *region = CTX_wm_region(&C);
  LayerGradientTarget target = layer_gradient_target_resolve(C, !ggd.image_editor);
  if (region == nullptr || !target.valid()) {
    return false;
  }
  const MaterialPaintLayerGradient &gradient = *target.gradient();

  if (ggd.image_editor) {
    /* A 2D editor can only show a UV row: object and generated coordinates have no texel. */
    if (gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
      return false;
    }
    ui::view2d_view_to_region_fl(
        &region->v2d, gradient.start[0], gradient.start[1], &ggd.start_px.x, &ggd.start_px.y);
    ui::view2d_view_to_region_fl(
        &region->v2d, gradient.end[0], gradient.end[1], &ggd.end_px.x, &ggd.end_px.y);
  }
  else {
    if (target.mesh == nullptr) {
      return false;
    }
    const bool cache_changed = ggd.cache_material != target.material ||
                               !BLI_uuid_equal(ggd.cache_marker, target.layer->marker) ||
                               ggd.cache_space != gradient.space ||
                               ggd.cache_object != target.object || ggd.cache_mesh != target.mesh;
    if (cache_changed) {
      ggd.cache_material = target.material;
      ggd.cache_marker = target.layer->marker;
      ggd.cache_space = gradient.space;
      ggd.cache_object = target.object;
      ggd.cache_mesh = target.mesh;
      ggd.start_tri = -1;
      ggd.end_tri = -1;
      ggd.start_point_cache = float3(FLT_MAX);
      ggd.end_point_cache = float3(FLT_MAX);
      ggd.start_found = false;
      ggd.end_found = false;
    }
    float projected[2];
    float3 point = float3(0.0f);

    const float3 start_point = float3(gradient.start);
    if (math::distance_squared(start_point, ggd.start_point_cache) == 0.0f && !ggd.start_found) {
      return false;
    }
    ggd.start_point_cache = start_point;
    ggd.start_found = paint_layer_gradient_world_point(
        *target.object, *target.mesh, gradient, start_point, ggd.start_tri, point);
    if (!ggd.start_found) {
      return false;
    }
    if (ED_view3d_project_float_global(region, point, projected, V3D_PROJ_TEST_CLIP_DEFAULT) !=
        V3D_PROJ_RET_OK)
    {
      return false;
    }
    ggd.start_px = float2(projected[0], projected[1]);

    const float3 end_point = float3(gradient.end);
    if (math::distance_squared(end_point, ggd.end_point_cache) == 0.0f && !ggd.end_found) {
      return false;
    }
    ggd.end_point_cache = end_point;
    ggd.end_found = paint_layer_gradient_world_point(
        *target.object, *target.mesh, gradient, end_point, ggd.end_tri, point);
    if (!ggd.end_found) {
      return false;
    }
    if (ED_view3d_project_float_global(region, point, projected, V3D_PROJ_TEST_CLIP_DEFAULT) !=
        V3D_PROJ_RET_OK)
    {
      return false;
    }
    ggd.end_px = float2(projected[0], projected[1]);
  }

  /* The line and handles show the row's ramp: the start disc at ramp 0, the midpoint marker at
   * the 50% color the midpoint bend defines, the end disc at ramp 1, and the arrow in the end
   * color. */
  /* The ramp is scene linear and the overlay is drawn in display space, so the colors are converted
   * the way brush colors are before they are drawn. */
  const auto overlay_color = [&](const float t) {
    float linear[4];
    BKE_paint_layers_gradient_color(gradient, t, linear);
    float display[3];
    IMB_colormanagement_scene_linear_to_srgb_v3(display, linear);
    return float4(display[0], display[1], display[2], linear[3]);
  };
  ggd.draw_options.ramp_colors = true;
  ggd.draw_options.color_start = overlay_color(0.0f);
  ggd.draw_options.color_mid = overlay_color(0.5f);
  ggd.draw_options.color_end = overlay_color(1.0f);

  ggd.midpoint = gradient.midpoint;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dragging
 * \{ */

void drag_finish(bContext &C, wmGizmo *gz, const bool commit)
{
  LayerGradientGizmoGroup &ggd = *group_data(gz);
  if (ggd.drag_handle == gradient_gizmo::NONE) {
    return;
  }

  MaterialPaintLayer *layer = nullptr;
  Material *material = paint_layer_gradient_target(&C, &layer);
  if (material != nullptr && layer != nullptr && layer->gradient != nullptr &&
      material == ggd.session_material && BLI_uuid_equal(layer->marker, ggd.session_marker))
  {
    if (commit) {
      /* Squared distances, so this is a few hundredths of a millimetre in object units. */
      constexpr float changed_epsilon = 1e-9f;
      const bool changed = math::distance_squared(float3(layer->gradient->start),
                                                  ggd.start_before) > changed_epsilon ||
                           math::distance_squared(float3(layer->gradient->end), ggd.end_before) >
                               changed_epsilon ||
                           std::abs(layer->gradient->midpoint - ggd.midpoint_before) >
                               changed_epsilon;
      if (changed) {
        ED_undo_push(&C, "Move Gradient Layer Handle");
      }
    }
    else {
      float start[3] = {ggd.start_before.x, ggd.start_before.y, ggd.start_before.z};
      float end[3] = {ggd.end_before.x, ggd.end_before.y, ggd.end_before.z};
      BKE_paint_layers_gradient_set_line(*material, *layer, start, end);
      BKE_paint_layers_gradient_set_midpoint(*material, *layer, ggd.midpoint_before);
    }
    notify_changed(C, *material);
  }

  ggd.drag_handle = gradient_gizmo::NONE;
  ggd.has_plane_hit = false;
  ggd.has_drag_plane = false;
}

void drag_begin(bContext &C, wmGizmo *gz, const wmEvent &event)
{
  LayerGradientGizmoGroup &ggd = *group_data(gz);
  LayerGradientTarget target = layer_gradient_target_resolve(C, !ggd.image_editor);
  if (!target.valid() || gz->highlight_part == gradient_gizmo::NONE) {
    return;
  }
  const MaterialPaintLayerGradient &gradient = *target.gradient();

  ggd.drag_handle = gz->highlight_part;
  ggd.start_before = float3(gradient.start);
  ggd.end_before = float3(gradient.end);
  ggd.midpoint_before = gradient.midpoint;
  ggd.session_material = target.material;
  ggd.session_marker = target.layer->marker;
  ggd.has_plane_hit = false;
  ggd.has_drag_plane = false;

  if (!ggd.image_editor && gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
    if (snap_enabled(C)) {
      /* Snapping: remember the press's surface point, the fallback for a drag that leaves the
       * mesh. */
      const float2 mval(float(event.mval[0]), float(event.mval[1]));
      ggd.has_plane_hit = paint_layer_gradient_hit_at_mouse(C, gradient, mval, ggd.plane_hit) &&
                          ggd.plane_hit.valid;
    }
    else if (ggd.drag_handle != gradient_gizmo::MID) {
      /* Free placement: drag in the view plane through the grabbed handle's current position. */
      int tri_cache = -1;
      const float3 handle_point = (ggd.drag_handle == gradient_gizmo::START) ?
                                      float3(gradient.start) :
                                      float3(gradient.end);
      ggd.has_drag_plane = paint_layer_gradient_world_point(
          *target.object, *target.mesh, gradient, handle_point, tri_cache, ggd.drag_plane_point);
    }
  }
}

void drag_apply(bContext &C, wmGizmo *gz, const wmEvent &event)
{
  LayerGradientGizmoGroup &ggd = *group_data(gz);
  MaterialPaintLayer *layer = nullptr;
  Material *material = paint_layer_gradient_target(&C, &layer);
  if (material == nullptr || layer == nullptr || layer->gradient == nullptr ||
      material != ggd.session_material || !BLI_uuid_equal(layer->marker, ggd.session_marker))
  {
    /* The row or its material changed mid-drag: stop without another write. */
    drag_finish(C, gz, false);
    return;
  }
  const MaterialPaintLayerGradient &gradient = *layer->gradient;
  const float2 mval(float(event.mval[0]), float(event.mval[1]));

  if (ggd.drag_handle == gradient_gizmo::MID) {
    /* The middle marker only moves along the line, exactly like the Color Gradient's. */
    const float2 axis = ggd.end_px - ggd.start_px;
    const float len_sq = math::length_squared(axis);
    if (len_sq >= 1.0f) {
      const float t = math::clamp(math::dot(mval - ggd.start_px, axis) / len_sq, 0.05f, 0.95f);
      if (BKE_paint_layers_gradient_set_midpoint(*material, *layer, t)) {
        notify_changed(C, *material);
      }
    }
    return;
  }

  float3 point = float3(0.0f);
  if (ggd.image_editor) {
    ARegion *region = CTX_wm_region(&C);
    if (region == nullptr || gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
      return;
    }
    float2 uv;
    ui::view2d_region_to_view(&region->v2d, mval.x, mval.y, &uv.x, &uv.y);
    point = float3(uv.x, uv.y, 0.0f);
  }
  else {
    LayerGradientTarget target = layer_gradient_target_resolve(C, true);
    if (!target.valid()) {
      drag_finish(C, gz, false);
      return;
    }
    if (!snap_enabled(C) && gradient.space != MA_PAINT_LAYER_GRADIENT_SPACE_UV) {
      /* Free placement: the cursor moves the handle in the view plane through the point the drag
       * started from, so the line can sit off the surface. */
      ARegion *region = CTX_wm_region(&C);
      const View3D *v3d = CTX_wm_view3d(&C);
      if (region == nullptr || v3d == nullptr || !ggd.has_drag_plane) {
        return;
      }
      float world[3];
      const float mval_fl[2] = {mval.x, mval.y};
      ED_view3d_win_to_3d(v3d, region, ggd.drag_plane_point, mval_fl, world);
      PaintLayerGradientHit free_hit;
      free_hit.position = math::transform_point(target.object->world_to_object(), float3(world));
      free_hit.valid = true;
      if (!paint_layer_gradient_point_from_hit(
              gradient, target.object, target.mesh, free_hit, point)) {
        return;
      }
    }
    else {
      /* Off the mesh, the press's tangent plane still gives a point in the row's space. */
      if (!paint_layer_gradient_point_at_mouse(C,
                                               gradient,
                                               target.object,
                                               target.mesh,
                                               mval,
                                               ggd.has_plane_hit ? &ggd.plane_hit : nullptr,
                                               point))
      {
        return;
      }
    }
  }

  float co[3] = {point.x, point.y, point.z};
  const bool changed = (ggd.drag_handle == gradient_gizmo::START) ?
                           BKE_paint_layers_gradient_set_start(*material, *layer, co) :
                           BKE_paint_layers_gradient_set_end(*material, *layer, co);
  if (changed) {
    notify_changed(C, *material);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gizmo callbacks
 * \{ */

void gizmo_draw(const bContext * /*C*/, wmGizmo *gz)
{
  const LayerGradientGizmoGroup &ggd = *group_data(gz);
  if (!ggd.points_valid) {
    return;
  }
  gradient_gizmo::draw(ggd.start_px, ggd.end_px, ggd.midpoint, ggd.draw_options);
}

int gizmo_test_select(bContext *C, wmGizmo *gz, const int mval[2])
{
  UNUSED_VARS(C);
  const LayerGradientGizmoGroup &ggd = *group_data(gz);
  if (!ggd.points_valid) {
    return -1;
  }
  /* The discs are small, so the pick area is wider than what is drawn; a miss drops the drag. */
  constexpr float pick_radius = gradient_gizmo::HANDLE_RADIUS * 1.8f;
  const int part = gradient_gizmo::hit_test(
      ggd.start_px, ggd.end_px, ggd.midpoint, float2(float(mval[0]), float(mval[1])), pick_radius);
  return (part == gradient_gizmo::NONE) ? -1 : part;
}

int gizmo_cursor_get(wmGizmo * /*gz*/)
{
  return WM_CURSOR_EDIT;
}

wmOperatorStatus gizmo_modal(bContext *C,
                             wmGizmo *gz,
                             const wmEvent *event,
                             eWM_GizmoFlagTweak /*tweak_flag*/)
{
  LayerGradientGizmoGroup &ggd = *group_data(gz);
  if (C == nullptr || event == nullptr || ggd.drag_handle == gradient_gizmo::NONE) {
    return OPERATOR_CANCELLED;
  }

  if (event->type == EVT_ESCKEY || (event->type == RIGHTMOUSE && event->val == KM_PRESS)) {
    drag_finish(*C, gz, false);
    return OPERATOR_CANCELLED;
  }
  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    drag_finish(*C, gz, true);
    return OPERATOR_FINISHED;
  }
  if (ELEM(event->type, MOUSEMOVE, INBETWEEN_MOUSEMOVE)) {
    drag_apply(*C, gz, *event);
    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL;
}

void gizmo_invoke_prepare(const bContext *C,
                          wmGizmoGroup * /*gzgroup*/,
                          wmGizmo *gz,
                          const wmEvent *event)
{
  if (C == nullptr || event == nullptr) {
    return;
  }
  drag_begin(*const_cast<bContext *>(C), gz, *event);
}

void group_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  LayerGradientGizmoGroup &ggd = *group_data(gzgroup);
  LayerGradientTarget target = layer_gradient_target_resolve(*C, !ggd.image_editor);
  if (ggd.drag_handle != gradient_gizmo::NONE && !target_is_session(target, ggd)) {
    /* The active row, its material or the object changed: the drag cannot write anymore. */
    drag_finish(*const_cast<bContext *>(C), ggd.gizmo, false);
  }
}

void group_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  LayerGradientGizmoGroup &ggd = *group_data(gzgroup);
  ggd.points_valid = points_update(*C, ggd);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Group setup
 * \{ */

bool group_poll_3d(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0 || CTX_wm_view3d(C) == nullptr) {
    return false;
  }
  LayerGradientTarget target = layer_gradient_target_resolve(*C, true);
  return target.valid() && target.gradient()->gizmo_shown;
}

bool group_poll_2d(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }
  const SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->mode != SI_MODE_PAINT) {
    return false;
  }
  LayerGradientTarget target = layer_gradient_target_resolve(*C, false);
  if (!target.valid() || !target.gradient()->gizmo_shown) {
    return false;
  }
  /* The handles may only edit the material the editor shows: a pinned image or another object's
   * composite would disagree with the active slot the panel edits. */
  if (ED_space_image_composite_material_get(CTX_data_main(C), sima->image) != target.material) {
    return false;
  }
  return target.gradient()->space == MA_PAINT_LAYER_GRADIENT_SPACE_UV;
}

void group_setup(const bContext *C, wmGizmoGroup *gzgroup, const bool image_editor)
{
  UNUSED_VARS(C);
  LayerGradientGizmoGroup *ggd = MEM_new<LayerGradientGizmoGroup>(__func__);
  ggd->image_editor = image_editor;
  /* The gradient's origin is the start disc: draw it larger than the end handle, and point an
   * arrow along the line. */
  ggd->draw_options.start_radius_scale = 0.9f;
  ggd->draw_options.direction_arrow = true;
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<LayerGradientGizmoGroup *>(customdata));
  };

  const wmGizmoType *gzt = WM_gizmotype_find("PAINT_GT_layer_gradient", true);
  if (gzt == nullptr) {
    return;
  }
  ggd->gizmo = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  WM_gizmo_set_fn_custom_modal(ggd->gizmo, gizmo_modal);
  /* Keep the handles on screen while one of them is dragged: a modal gizmo is otherwise skipped
   * by #wm_gizmo_is_visible, which is why the standard gizmo types set this flag in their own
   * setup (#GIZMO_GT_cage_2d does). */
  WM_gizmo_set_flag(ggd->gizmo, WM_GIZMO_DRAW_MODAL, true);
}

void group_setup_3d(const bContext *C, wmGizmoGroup *gzgroup)
{
  group_setup(C, gzgroup, false);
}

void group_setup_2d(const bContext *C, wmGizmoGroup *gzgroup)
{
  group_setup(C, gzgroup, true);
}

void paint_layer_gradient_gizmo_type_setup(wmGizmoType *gzt)
{
  gzt->idname = "PAINT_GT_layer_gradient";
  gzt->struct_size = sizeof(wmGizmo);

  gzt->draw = gizmo_draw;
  gzt->test_select = gizmo_test_select;
  gzt->cursor_get = gizmo_cursor_get;
}

}  // namespace

/** \} */

void ED_gizmotypes_paint_layer_gradient()
{
  /* Every space type's gizmo callback runs at startup and both editor families ask for the type;
   * the first call registers it. */
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  WM_gizmotype_append(paint_layer_gradient_gizmo_type_setup);
}

void ED_view3d_paint_layer_gradient_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Gradient Layer Handles";
  gzgt->idname = "VIEW3D_GGT_paint_layer_gradient";

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->poll = group_poll_3d;
  gzgt->setup = group_setup_3d;
  gzgt->invoke_prepare = gizmo_invoke_prepare;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = group_refresh;
  gzgt->draw_prepare = group_draw_prepare;
}

void ED_image_paint_layer_gradient_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Gradient Layer Handles";
  gzgt->idname = "IMAGE_GGT_paint_layer_gradient";

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->poll = group_poll_2d;
  gzgt->setup = group_setup_2d;
  gzgt->invoke_prepare = gizmo_invoke_prepare;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = group_refresh;
  gzgt->draw_prepare = group_draw_prepare;
}

}  // namespace blender
