/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * 3D Viewport frontend of the shape drawing tools (Sculpt Mode, PBR Paint).
 *
 * The shape is built in region pixels of the 3D view, with the same drag builders and Bézier
 * input as the Image Editor tools (`paint_image_shape_ops.cc`): Shift snaps, Alt grows from the
 * center, Space moves the in-progress shape, a plain click places the default-size shape, and
 * Polyline / Curve Patch place points one click at a time.
 *
 * Writing into the mesh is delegated to `sculpt_paint_shape.cc`: the PBR vertex-attribute and
 * color-attribute canvases bake there (one sculpt undo step per shape), and the Material
 * channel maps / Image canvas bake through the PBVH pixels (one image undo step per shape).
 */

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <optional>

#include "MEM_guardedalloc.h"

#include "BLI_kdopbvh.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_util_modal_multiwin.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "../paint_bezier_input.hh"
#include "../paint_intern.hh"
#include "../paint_shape.hh"
#include "../paint_shape_create.hh"
#include "../paint_shape_draw.hh"
#include "../paint_shape_edit.hh"
#include "../paint_shape_op_props.hh"
#include "../paint_shape_raster.hh"
#include "../paint_shape_space.hh"
#include "../paint_vector_editor.hh"
#include "paint_image_shape_composite.hh"
#include "paint_shape_vector_3d.hh"
#include "sculpt_intern.hh"
#include "sculpt_paint_shape.hh"

namespace blender::ed::sculpt_paint {

namespace shape_3d {

/* -------------------------------------------------------------------- */
/** \name Constants
 * \{ */

/** Flattening error (region pixels) of the preview outline. */
constexpr float SHAPE_PREVIEW_ERROR_PX = 1.0f;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bézier input mapping (region pixels)
 * \{ */

static float2 shape_input_to_region(const ARegion & /*region*/, const float2 &co)
{
  return co;
}

static float2 shape_input_event_to_user(const ARegion & /*region*/, const wmEvent &event)
{
  return float2(event.mval[0], event.mval[1]);
}

/* The 3D frontend works in region pixels directly: the shape is projected onto the mesh only
 * when it is written, so the input needs no view transform. */
constexpr bezier_input::Mapping SHAPE_INPUT_MAPPING = {shape_input_to_region,
                                                       shape_input_event_to_user};

/**
 * Live SurfaceAnchored creation state (R8). `bezier_input::Mapping` is plain function pointers
 * with no userdata, so the anchor frame is held here for the mapping callbacks while the creation
 * modal is alive. Single shape creation at a time on the main thread, so one file-scope context is
 * enough.
 */
struct SurfaceCreationContext {
  bool active = false;
  /** Object-space region pixels per object unit (see the scale note in the invoke). */
  float px_per_unit = 1.0f;
  float4x4 object_to_world = float4x4::identity();
  float4x4 world_to_object = float4x4::identity();
  float3 anchor_object = float3(0.0f);
  float3 tangent_object = float3(1.0f, 0.0f, 0.0f);
  float3 bitangent_object = float3(0.0f, 1.0f, 0.0f);
  /** World plane `(n, d)` with `dot(n, p) + d = 0`, for the event ray intersection. */
  float plane_world[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};
static SurfaceCreationContext g_surface_ctx;

/** Figure (plane-pixel) point -> region pixels through the *current* view, for the preview. */
static float2 shape_input_surface_to_region(const ARegion &region, const float2 &co)
{
  if (!g_surface_ctx.active) {
    return co;
  }
  const float inv_px = 1.0f / g_surface_ctx.px_per_unit;
  const float3 co_object = g_surface_ctx.anchor_object +
                           g_surface_ctx.tangent_object * (co.x * inv_px) +
                           g_surface_ctx.bitangent_object * (co.y * inv_px);
  const float3 co_world = math::transform_point(g_surface_ctx.object_to_world, co_object);
  const float co_world_v[3] = {co_world.x, co_world.y, co_world.z};
  float r[2];
  if (ED_view3d_project_float_global(&region, co_world_v, r, V3D_PROJ_TEST_CLIP_NEAR) !=
      V3D_PROJ_RET_OK)
  {
    return co;
  }
  return float2(r);
}

/** Event position -> figure (plane-pixel) point: intersect the view ray with the anchor plane. */
static float2 shape_input_surface_event_to_user(const ARegion &region, const wmEvent &event)
{
  const float2 fallback = float2(event.mval[0], event.mval[1]);
  if (!g_surface_ctx.active) {
    return fallback;
  }
  const float mval[2] = {float(event.mval[0]), float(event.mval[1])};
  float world[3];
  if (!ED_view3d_win_to_3d_on_plane(&region, g_surface_ctx.plane_world, mval, false, world)) {
    return fallback;
  }
  /* Work in object space: the shape, project_point and the anchor are all object space. */
  const float3 co_object = math::transform_point(g_surface_ctx.world_to_object,
                                                 float3(world[0], world[1], world[2]));
  const float3 d = co_object - g_surface_ctx.anchor_object;
  return float2(math::dot(d, g_surface_ctx.tangent_object),
                math::dot(d, g_surface_ctx.bitangent_object)) *
         g_surface_ctx.px_per_unit;
}

constexpr bezier_input::Mapping SHAPE_SURFACE_INPUT_MAPPING = {
    shape_input_surface_to_region, shape_input_surface_event_to_user};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator state
 * \{ */

struct ShapeState {
  ARegion *owner_region = nullptr;
  ARegionType *owner_region_type = nullptr;
  void *draw_handle = nullptr;

  /** The shared creation machine (drag + Bézier input); constructed at invoke. */
  std::optional<shape::ShapeCreateGesture> create;

  /** How the shape is authored and applied - frozen view, or anchored to the surface. */
  bool surface = false;
  shape::ShapeSpaceDesc space_desc;
  /** Mapping the gesture works through; surface mode uses the anchor-plane mapping. */
  bezier_input::Mapping mapping = SHAPE_INPUT_MAPPING;

  /** Vector-mode live session: the modal drives it through #vector_editor instead of #create. */
  bool vector_edit = false;
  /** Shared gesture state machine of the live session. */
  shape::ShapeVectorEditor vector_editor;
};

static void shape_state_free(bContext *C, wmOperator *op)
{
  ShapeState *state = static_cast<ShapeState *>(op->customdata);
  if (state == nullptr) {
    return;
  }
  /* A live session driven by this modal is no longer modal-active once the operator ends. */
  if (C != nullptr) {
    if (Object *ob = CTX_data_active_object(C)) {
      if (shape::PaintShapeSession *session = shape::paint_shape_session_get(*ob)) {
        shape::paint_shape_session_modal_set_active(*session, false);
      }
    }
  }
  g_surface_ctx.active = false;
  if (state->draw_handle && state->owner_region_type) {
    ED_region_draw_cb_exit(state->owner_region_type, state->draw_handle);
  }
  if (state->owner_region) {
    ED_region_tag_redraw(state->owner_region);
  }
  MEM_delete(state);
  op->customdata = nullptr;
  if (wmWindow *win = CTX_wm_window(C)) {
    WM_cursor_modal_restore(win);
  }
  ED_workspace_status_text(C, nullptr);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Drawing
 * \{ */

static void shape_draw_drag(const bContext * /*C*/, ARegion *region, void *arg)
{
  const ShapeState *state = static_cast<const ShapeState *>(arg);
  /* Every 3D view shares the region type: only the invoking view shows the outline. */
  if (state == nullptr || !state->create || !state->create->has_shape() ||
      !state->create->moved() || region != state->owner_region)
  {
    return;
  }
  /* View space: the shape is already in region pixels. Surface space: plane pixels -> region
   * through the current view (the same mapping the gesture authored in). */
  /* While F adjusts the width, the stroke edges are drawn too, so the band is visible. */
  const bool show_width = state->create->value_drag_active() &&
                          state->create->value_drag_is_width();
  shape::shape_draw_creation_preview(
      state->create->shape(),
      SHAPE_PREVIEW_ERROR_PX,
      [state, region](const float2 &p) { return state->mapping.to_region(*region, p); },
      show_width ? &state->create->style() : nullptr);
}

static void shape_draw_input(const bContext * /*C*/, ARegion *region, void *arg)
{
  const ShapeState *state = static_cast<const ShapeState *>(arg);
  if (state == nullptr || !state->create || region != state->owner_region) {
    return;
  }
  state->create->input().draw(*region, state->mapping, bezier_input::DrawStyle());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator callbacks
 * \{ */

/** Freeze the 3D viewport into a serializable space description, so `exec` / Redo (F9) rebuild
 * the same projection instead of reading the live view. This is the one place the 3D frontend
 * reads rv3d; every backend works from the #ShapeSpace. */
static shape::ShapeSpaceDesc shape_space_desc_from_view(ARegion &region,
                                                        Object &ob,
                                                        const View3D *v3d)
{
  shape::ShapeSpaceDesc desc;
  desc.type = shape::ShapeSpaceType::ViewProjector;
  RegionView3D *rv3d = static_cast<RegionView3D *>(region.regiondata);
  BLI_assert(rv3d != nullptr);
  if (rv3d == nullptr) {
    return desc;
  }
  ED_view3d_init_mats_rv3d(&ob, rv3d);
  desc.persmat = float4x4(rv3d->persmatob);
  desc.viewinv = float4x4(rv3d->viewinv);
  desc.is_persp = rv3d->is_persp != 0;
  desc.win_size = int2(region.winx, region.winy);
  /* The clip range lives on the view, not the region-view (its perspective flag is elsewhere). */
  if (v3d != nullptr) {
    desc.clip_start = v3d->clip_start;
    desc.clip_end = v3d->clip_end;
  }
  return desc;
}

/** Finish the gesture with the built shape. */
static wmOperatorStatus shape_apply(bContext *C, wmOperator *op, ShapeState *state)
{
  const shape::ShapeCreateGesture &create = *state->create;
  const shape::PaintShape shape = create.has_shape() ? create.shape() : create.shape_from_input();
  const shape::ShapeStyle style = create.style();
  ARegion *region = state->owner_region;
  Object *ob = CTX_data_active_object(C);
  Scene *scene = CTX_data_scene(C);

  if (shape.is_empty() || region == nullptr) {
    shape_state_free(C, op);
    BKE_report(op->reports, RPT_WARNING, "Paint Shape: nothing to draw");
    return OPERATOR_CANCELLED;
  }
  if (ob == nullptr || scene == nullptr || scene->toolsettings == nullptr) {
    shape_state_free(C, op);
    BKE_report(op->reports, RPT_WARNING, "Paint Shape: no active object to paint on");
    return OPERATOR_CANCELLED;
  }

  /* The space must be read from the state before it is freed. Surface mode keeps its anchor;
   * view mode freezes the current viewport. */
  shape::ShapeSpaceDesc space_desc;
  if (state->surface) {
    space_desc = state->space_desc;
    /* Depth rejection: half the shape's larger half-axis, in object units (px_per_unit is object
     * space), with a floor. */
    const rctf bounds = shape::shape_bounds_calc(shape, style);
    const float2 size_px = float2(bounds.xmax - bounds.xmin, bounds.ymax - bounds.ymin);
    const float half_object = 0.5f * math::length(size_px) / space_desc.anchor.px_per_unit;
    space_desc.anchor.max_depth = std::max(shape::SURFACE_ANCHOR_MIN_DEPTH,
                                           shape::SURFACE_ANCHOR_MAX_DEPTH_FACTOR * half_object);
  }
  else {
    space_desc = shape_space_desc_from_view(*region, *ob, CTX_wm_view3d(C));
  }
  shape_state_free(C, op);

  /* Store the draw so `exec` / Redo (F9) can replay it, and apply it through the shared path. */
  shape::shape_to_op_props(op, shape);
  shape::style_to_op_props(op, style);
  shape::space_to_op_props(op, space_desc);

  const Vector<shape::PaintShape> shapes = {shape};

  /* Vector mode: keep the shape live on the tangent plane instead of baking it. Vector is always
   * SurfaceAnchored (the Projection choice is Pixel-only). The operator ends CANCELLED so it does
   * not become the redo anchor; the session's Apply is its own undo step. */
  if (style.draw_mode == PAINT_SHAPE_DRAW_VECTOR &&
      space_desc.type == shape::ShapeSpaceType::SurfaceAnchored &&
      shape::shape_canvas_supports_preview(*scene->toolsettings))
  {
    shape::paint_shape_session_begin(C, op, *ob, space_desc, shape, style);
    ED_region_tag_redraw(region);
    return OPERATOR_CANCELLED;
  }

  if (shape::shape_apply_3d(C, op, *ob, space_desc, shapes, style)) {
    ED_region_tag_redraw(region);
    return OPERATOR_FINISHED;
  }
  /* Nothing was written (the backend reported the reason); do not become an F9 anchor. */
  return OPERATOR_CANCELLED;
}

/** Contour / handle grab tolerance of the live Vector session, in region pixels. */
static float shape_hit_tolerance_px()
{
  return shape::SHAPE_HIT_TOLERANCE_PX * UI_SCALE_FAC;
}

/** Status bar of a live Vector session: the same icon items as the Image Editor session (the keys
 * come from the modal keymap, so a remap shows up), plus the Ctrl+RMB contour cut. */
static void shape3d_status_set_edit(bContext *C, const wmOperatorType *ot)
{
  WorkspaceStatus status(C);
  status.item(IFACE_("Select / Move"), ICON_MOUSE_LMB);
  status.item(IFACE_("Cut Contour"), ICON_EVENT_CTRL, ICON_MOUSE_RMB);
  if (ot != nullptr) {
    status.opmodal(IFACE_("Grab"), ot, PAINT_SHAPE_MODAL_MOVE);
    status.opmodal(IFACE_("Rotate"), ot, PAINT_SHAPE_MODAL_ROTATE);
    status.opmodal(IFACE_("Scale"), ot, PAINT_SHAPE_MODAL_SCALE);
    status.opmodal(IFACE_("Stroke Width"), ot, PAINT_SHAPE_MODAL_STROKE_WIDTH);
    status.opmodal(IFACE_("Strength"), ot, PAINT_SHAPE_MODAL_STROKE_OPACITY);
    status.opmodal(IFACE_("Extrude"), ot, PAINT_SHAPE_MODAL_EXTRUDE);
    status.opmodal(IFACE_("Next Shape"), ot, PAINT_SHAPE_MODAL_SELECT_NEXT);
    status.opmodal(IFACE_("Undo"), ot, PAINT_SHAPE_MODAL_UNDO);
    status.opmodal(IFACE_("Redo"), ot, PAINT_SHAPE_MODAL_REDO);
  }
  status.item(IFACE_("Swap Colors"), ICON_EVENT_X);
  status.item(IFACE_("Apply"), ICON_EVENT_RETURN);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
}

static wmOperatorStatus shape_draw_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  Scene *scene = CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (!region || !scene || !scene->toolsettings || !depsgraph) {
    return OPERATOR_CANCELLED;
  }

  /* Ctrl+RMB cuts the contour of the live Vector session (inserts a point into the segment under
   * the cursor). Without a session, or while the modal drives it (it handles the event itself),
   * the event is left to whatever else listens. */
  if (event->type == RIGHTMOUSE) {
    Object *ob = CTX_data_active_object(C);
    shape::PaintShapeSession *session = ob != nullptr ? shape::paint_shape_session_get(*ob) :
                                                        nullptr;
    if (session == nullptr || shape::paint_shape_session_modal_active(*session)) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }
    std::unique_ptr<shape::VectorEditHost> host = shape::paint_shape_session_host_create(
        *C, *session, shape_hit_tolerance_px());
    float2 press_px;
    if (!shape::paint_shape_session_event_to_shape_px(*session, *region, *event, press_px)) {
      return OPERATOR_CANCELLED;
    }
    shape::ShapeVectorEditor editor;
    editor.handle_event(C, *event, press_px, true, *host);
    ED_region_tag_redraw(region);
    return OPERATOR_CANCELLED;
  }

  /* F over a live Vector session adjusts its own stroke width: the radial control edits the shared
   * settings block, which the session's private copy never reads. The modal takes over, so the
   * width follows the cursor until a click confirms (the modal map's F item). */
  if (event->type == EVT_FKEY) {
    Object *ob = CTX_data_active_object(C);
    shape::PaintShapeSession *session = ob != nullptr ? shape::paint_shape_session_get(*ob) :
                                                        nullptr;
    if (session == nullptr || shape::paint_shape_session_modal_active(*session)) {
      return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
    }
    auto *state = MEM_new<ShapeState>(__func__);
    state->owner_region = region;
    state->owner_region_type = region->runtime->type;
    state->vector_edit = true;
    std::unique_ptr<shape::VectorEditHost> host = shape::paint_shape_session_host_create(
        *C, *session, shape_hit_tolerance_px());
    state->vector_editor.begin_value_gesture(
        *host, true, int2(event->mval[0], event->mval[1]));
    shape::paint_shape_session_modal_set_active(*session, true);
    op->customdata = state;
    WM_event_add_modal_handler(C, op);
    WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
    shape3d_status_set_edit(C, op->type);
    return OPERATOR_RUNNING_MODAL;
  }

  /* Create the missing PBR channel maps once, outside any undo step, so the bake itself never
   * allocates images. Sculpt Mode's channels belong to the Sculpt paint (its brush and visible
   * channel set), not the Image Editor's image-paint brush -- same source #SCULPT_OT_color_gradient
   * uses. */
  if (Object *ob = CTX_data_active_object(C)) {
    if (scene->toolsettings->sculpt != nullptr) {
      shape::composite_targets_ensure_writable(C,
                                               ob,
                                               scene->toolsettings->sculpt->paint,
                                               BKE_paint_shape_settings_get(*scene->toolsettings));
    }
  }

  auto *state = MEM_new<ShapeState>(__func__);
  state->owner_region = region;
  state->owner_region_type = region->runtime->type;

  shape::ShapeStyle style = shape::style_from_settings(
      BKE_paint_shape_settings_get(*scene->toolsettings));
  if (scene->toolsettings->sculpt != nullptr) {
    shape::style_brush_values_from_brush(scene->toolsettings->sculpt->paint, style);
  }

  /* Vector mode with a live session on the active object: this invocation becomes a move drag on
   * it (mirrors the Image Editor). A press outside the shape confirms the session; a press while
   * the modal already drives it is ignored. Only the Image canvas has a live preview. */
  if (style.draw_mode == PAINT_SHAPE_DRAW_VECTOR &&
      shape::shape_canvas_supports_preview(*scene->toolsettings))
  {
    Object *ob = CTX_data_active_object(C);
    shape::PaintShapeSession *session = ob != nullptr ? shape::paint_shape_session_get(*ob) :
                                                        nullptr;
    if (session != nullptr) {
      if (shape::paint_shape_session_modal_active(*session)) {
        MEM_delete(state);
        return OPERATOR_CANCELLED;
      }
      std::unique_ptr<shape::VectorEditHost> host = shape::paint_shape_session_host_create(
          *C, *session, shape_hit_tolerance_px());
      float2 press_px;
      if (!shape::paint_shape_session_event_to_shape_px(*session, *region, *event, press_px)) {
        /* Ray parallel to the plane: keep the last valid point, treat as a miss (confirms). */
        press_px = float2(event->mval[0], event->mval[1]);
      }
      if (!state->vector_editor.start_move_pick(C, press_px, *host)) {
        /* A press outside the shape confirms the session (bakes it). */
        MEM_delete(state);
        shape::paint_shape_session_commit(C, *ob);
        return OPERATOR_CANCELLED;
      }
      state->vector_edit = true;
      shape::paint_shape_session_modal_set_active(*session, true);
      op->customdata = state;
      WM_event_add_modal_handler(C, op);
      WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_NSEW_SCROLL);
      shape3d_status_set_edit(C, op->type);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  /* Projection ("space" tool property, shown as Projection for Draw Mode = Pixel): 0 = view
   * (ViewProjector, default), 1 = surface (SurfaceAnchored). Vector always uses Surface, so its
   * live session can anchor the cage to the tangent plane. A Surface creation raycasts the surface
   * under the first press and a hit failure is a refusal. */
  int space_mode = RNA_enum_get(op->ptr, "space");
  if (style.draw_mode == PAINT_SHAPE_DRAW_VECTOR) {
    space_mode = 1;
  }
  if (space_mode == 1) {
    Object *ob = CTX_data_active_object(C);
    View3D *v3d = CTX_wm_view3d(C);
    RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
    std::optional<CursorGeometryInfo> hit;
    const Sculpt *sculpt = CTX_data_tool_settings(C) ? CTX_data_tool_settings(C)->sculpt : nullptr;
    if (ob != nullptr && sculpt != nullptr) {
      /* resolve_hit_object = false: the shape anchors to the operator's active object, and the
       * scalar-updating side effects on the sculpt cursor must not switch it to another object. */
      ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);
      hit = cursor_geometry_info_update(*depsgraph,
                                        sculpt->paint,
                                        sculpt,
                                        vc,
                                        CTX_data_active_base(C),
                                        float2(event->mval[0], event->mval[1]),
                                        false,
                                        false,
                                        nullptr);
    }
    if (ob == nullptr || v3d == nullptr || rv3d == nullptr || !hit) {
      MEM_delete(state);
      BKE_report(op->reports, RPT_WARNING, "Paint Shape: no surface under the cursor");
      return OPERATOR_CANCELLED;
    }
    ED_view3d_init_mats_rv3d(ob, rv3d);
    const float4x4 object_to_world(ob->object_to_world());
    const float4x4 world_to_object = math::invert(object_to_world);
    const float3 anchor_object = hit->location;
    const float3 normal_object = math::normalize(hit->normal);
    /* Tangent = screen horizontal (viewinv X) in object space; the shared frame
     * (#surface_anchor_frame) orthonormalizes it against the normal, so the authored pixels are
     * the same basis the space projection and the cage use. */
    const float3 view_x_world = float3(
        rv3d->viewinv[0][0], rv3d->viewinv[0][1], rv3d->viewinv[0][2]);
    const shape::SurfaceAnchorFrame frame = shape::surface_anchor_frame(
        normal_object, math::transform_direction(world_to_object, view_x_world));
    const float3 tangent_object = frame.tangent;
    const float3 bitangent_object = frame.bitangent;

    /* Pixels per OBJECT unit along the plane axes: the shape lives in object space, so a world
     * px_per_unit must be scaled by how many world units one object unit spans. With a
     * non-uniform object scale use the mean of the two axes (same rule as the stroke width, O2). */
    const float3 anchor_world = math::transform_point(object_to_world, anchor_object);
    const float anchor_world_v[3] = {anchor_world.x, anchor_world.y, anchor_world.z};
    const float pixel_size_world = ED_view3d_pixel_size(rv3d, anchor_world_v);
    const float px_per_unit_world = 1.0f / std::max(pixel_size_world, 1e-6f);
    const float px_per_unit = shape::surface_anchor_px_per_unit(
        px_per_unit_world, object_to_world, tangent_object, bitangent_object);

    /* World plane normal from the inverse-transpose (= transpose of world_to_object), not the
     * position transform, so a non-uniform scale does not tilt the plane. */
    const float3 normal_world = math::normalize(
        math::transform_direction(math::transpose(world_to_object), normal_object));

    state->surface = true;
    state->space_desc.type = shape::ShapeSpaceType::SurfaceAnchored;
    state->space_desc.anchor.co = anchor_object;
    state->space_desc.anchor.normal = normal_object;
    state->space_desc.anchor.tangent = tangent_object;
    state->space_desc.anchor.px_per_unit = px_per_unit;
    state->space_desc.persmat = float4x4(rv3d->persmatob);
    state->space_desc.viewinv = float4x4(rv3d->viewinv);
    state->space_desc.is_persp = rv3d->is_persp != 0;
    state->space_desc.win_size = int2(region->winx, region->winy);
    state->space_desc.clip_start = v3d->clip_start;
    state->space_desc.clip_end = v3d->clip_end;
    state->mapping = SHAPE_SURFACE_INPUT_MAPPING;

    /* UV / triangle restoration cache of the anchor. The face under the cursor (set by the cursor
     * raycast above) narrows the triangle search, so a press does not build a BVH of the mesh. */
    shape::SurfaceAnchor &anchor = state->space_desc.anchor;
    const SculptSession *ss = ob->runtime ? ob->runtime->sculpt_session : nullptr;
    const int hit_face = (ss != nullptr) ? ss->active_face_index.value_or(-1) : -1;
    shape::surface_anchor_cache_fill(*ob, *depsgraph, hit_face, anchor);

    g_surface_ctx.active = true;
    g_surface_ctx.px_per_unit = px_per_unit;
    g_surface_ctx.object_to_world = object_to_world;
    g_surface_ctx.world_to_object = world_to_object;
    g_surface_ctx.anchor_object = anchor_object;
    g_surface_ctx.tangent_object = tangent_object;
    g_surface_ctx.bitangent_object = bitangent_object;
    g_surface_ctx.plane_world[0] = normal_world.x;
    g_surface_ctx.plane_world[1] = normal_world.y;
    g_surface_ctx.plane_world[2] = normal_world.z;
    g_surface_ctx.plane_world[3] = -math::dot(normal_world, anchor_world);
  }

  /* The tool's type property overrides the settings; -1 means "use the settings' type". */
  int type = RNA_enum_get(op->ptr, "type");
  if (type < 0) {
    type = BKE_paint_shape_settings_get(*scene->toolsettings).type;
  }
  state->create.emplace(ePaintShapeType(type), style, state->mapping);

  const bool button_held = (event->type == LEFTMOUSE) && (event->val == KM_PRESS);
  /* The press point is in the gesture's space: region pixels for view, plane pixels for surface. */
  state->create->begin(*region,
                       *event,
                       state->mapping.event_to_user(*region, *event),
                       button_held);

  if (state->create->use_input()) {
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, shape_draw_input, state, REGION_DRAW_POST_PIXEL);
    shape::shape_status_set_input(C);
  }
  else {
    state->draw_handle = ED_region_draw_cb_activate(
        state->owner_region_type, shape_draw_drag, state, REGION_DRAW_POST_PIXEL);
    shape::shape_status_set_creation(C, state->create->type());
  }

  op->customdata = state;
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus shape_draw_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  ShapeState *state = static_cast<ShapeState *>(op->customdata);
  /* A window-level modal keeps the region of its invoke in the context, so a click in the tool
   * header or the N-panel would look like a click in the viewport (in a live Vector session that
   * means "press outside the shape": Apply). The tracker resolves the region really under the
   * cursor, the way the Curve Patch editing modal does. */
  ed::ModalViewportTracker tracker(*C, *event, SPACE_VIEW3D, RGN_TYPE_WINDOW);
  ARegion *region = CTX_wm_region(C);
  if (state == nullptr || region == nullptr) {
    shape_state_free(C, op);
    return OPERATOR_CANCELLED;
  }
  {
    /* Off the viewport the event belongs to the UI (settings edits keep the shape alive), except
     * modal-map actions and an in-flight gesture, which must not be orphaned by leaving the view. */
    bool busy = false;
    if (state->vector_edit) {
      busy = state->vector_editor.gesture() != shape::ShapeVectorEditor::Gesture::None;
    }
    else if (state->create) {
      busy = state->create->value_drag_active() ||
             (!state->create->use_input() && state->create->has_shape());
    }
    if (!tracker.found() && event->type != EVT_MODAL_MAP && !busy) {
      return OPERATOR_PASS_THROUGH;
    }
  }

  /* Vector-mode live session: the shared gesture machine edits it; the operator status, undo and
   * the commit/cancel indirection stay here. */
  if (state->vector_edit) {
    Object *ob = CTX_data_active_object(C);
    shape::PaintShapeSession *session = ob != nullptr ? shape::paint_shape_session_get(*ob) :
                                                        nullptr;
    if (session == nullptr) {
      /* The session ended underneath (takeover, teardown). */
      shape_state_free(C, op);
      return OPERATOR_CANCELLED;
    }
    /* The owner region was closed or re-typed: its pointer is dangling, so cancel the session
     * instead of tagging / drawing through it. */
    if (!shape::paint_shape_session_region_alive(*session)) {
      shape_state_free(C, op);
      shape::paint_shape_session_cancel(C, *ob);
      return OPERATOR_CANCELLED;
    }
    /* The active tool changed (a toolbar click): the session must end -- settle it (Apply, or
     * Cancel when the anchored surface cannot be restored) and let the event reach the new tool
     * instead of being swallowed by this window-level modal. */
    const bToolRef *tool = WM_toolsystem_ref_from_context(C);
    if (tool != nullptr && !shape::paint_shape_session_tool_matches(*session, tool->idname)) {
      shape_state_free(C, op);
      shape::paint_shape_session_settle_forced(C, *ob);
      return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
    }
    const bool in_owner_region = (region == state->owner_region) &&
                                 region->regiontype == RGN_TYPE_WINDOW;
    std::unique_ptr<shape::VectorEditHost> host = shape::paint_shape_session_host_create(
        *C, *session, shape_hit_tolerance_px());
    float2 event_px = state->vector_editor.cursor_px();
    if (in_owner_region &&
        !shape::paint_shape_session_event_to_shape_px(*session, *region, *event, event_px))
    {
      /* Ray parallel to the plane: keep the last valid point. */
      event_px = state->vector_editor.cursor_px();
    }
    /* A gizmo handle drag settles during a redraw, where the deferred (Material / Image) preview
     * cannot be composited; do it from the next event, before anything else reads the session. */
    if (state->vector_editor.gesture() == shape::ShapeVectorEditor::Gesture::None) {
      host->flush_deferred_preview(C);
    }

    auto session_undo = [&]() -> wmOperatorStatus {
      const bool stepped = state->vector_editor.undo(C, *host);
      if (wmWindow *win = CTX_wm_window(C)) {
        WM_cursor_modal_restore(win);
      }
      if (stepped) {
        ED_region_tag_redraw(region);
        shape3d_status_set_edit(C, op->type);
        return OPERATOR_RUNNING_MODAL;
      }
      /* Nothing left in the session history: discard the whole session and consume the key, so
       * the global undo does not also fire (it would cancel the session and pop the user's
       * previous action in one step). */
      shape_state_free(C, op);
      shape::paint_shape_session_cancel(C, *ob);
      return OPERATOR_CANCELLED;
    };
    auto session_redo = [&]() -> wmOperatorStatus {
      const bool stepped = state->vector_editor.redo(C, *host);
      if (wmWindow *win = CTX_wm_window(C)) {
        WM_cursor_modal_restore(win);
      }
      if (stepped) {
        ED_region_tag_redraw(region);
        shape3d_status_set_edit(C, op->type);
        return OPERATOR_RUNNING_MODAL;
      }
      return OPERATOR_PASS_THROUGH;
    };

    if (event->type == EVT_MODAL_MAP &&
        (event->val == PAINT_SHAPE_MODAL_UNDO || event->val == PAINT_SHAPE_MODAL_REDO))
    {
      return (event->val == PAINT_SHAPE_MODAL_REDO) ? session_redo() : session_undo();
    }
    if (event->val == KM_PRESS && (event->modifier & (KM_CTRL | KM_OSKEY)) != 0 &&
        ELEM(event->type, EVT_ZKEY, EVT_YKEY))
    {
      if (event->type == EVT_YKEY || (event->modifier & KM_SHIFT) != 0) {
        return session_redo();
      }
      return session_undo();
    }
    if (event->type == EVT_MODAL_MAP && event->val == PAINT_SHAPE_MODAL_AXIS_X &&
        !ELEM(state->vector_editor.gesture(),
              shape::ShapeVectorEditor::Gesture::MovePick,
              shape::ShapeVectorEditor::Gesture::MoveActive,
              shape::ShapeVectorEditor::Gesture::Scale))
    {
      WM_operator_name_call(
          C, "PAINT_OT_shape_colors_swap", wm::OpCallContext::ExecDefault, nullptr, nullptr);
      host->restamp(C);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }

    /* Navigation (orbit / pan / zoom / trackpad / NDOF / numpad view / view pie) passes straight
     * through: the SurfaceAnchored session follows the live view (overlay and cage reproject), so
     * no Apply is needed. Unrecognised events reach the view keymap via #handle_event returning
     * Unhandled, or via the explicit default below. */

    /* A press on the active cage belongs to the transform gizmo (handle drags), not to the
     * modal's own move/point gestures. Passing the event through lets the gizmo's own modal own
     * it, exactly like the Image Editor and the selection transform. */
    if (event->type == LEFTMOUSE && event->val == KM_PRESS && in_owner_region &&
        state->vector_editor.gesture() == shape::ShapeVectorEditor::Gesture::None &&
        ED_view3d_shape_transform_gizmo_hit(C, event->mval))
    {
      return OPERATOR_PASS_THROUGH;
    }

    switch (state->vector_editor.handle_event(C, *event, event_px, in_owner_region, *host)) {
      case shape::ShapeVectorEditor::Status::Handled: {
        if (state->vector_editor.gesture() == shape::ShapeVectorEditor::Gesture::None) {
          if (wmWindow *win = CTX_wm_window(C)) {
            WM_cursor_modal_restore(win);
          }
          shape3d_status_set_edit(C, op->type);
        }
        else if (wmWindow *win = CTX_wm_window(C)) {
          WM_cursor_modal_set(win, WM_CURSOR_NSEW_SCROLL);
        }
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      case shape::ShapeVectorEditor::Status::Unhandled:
        return OPERATOR_PASS_THROUGH;
      case shape::ShapeVectorEditor::Status::Finished:
        shape_state_free(C, op);
        shape::paint_shape_session_commit(C, *ob);
        ED_region_tag_redraw(region);
        return OPERATOR_FINISHED;
      case shape::ShapeVectorEditor::Status::Cancelled:
        shape_state_free(C, op);
        shape::paint_shape_session_cancel(C, *ob);
        ED_region_tag_redraw(region);
        return OPERATOR_CANCELLED;
    }
    return OPERATOR_RUNNING_MODAL;
  }

  shape::ShapeCreateGesture &create = *state->create;

  /* Pixel F / Shift+F: adjust the stroke width / the brush strength (the shape's overall
   * opacity), like the Image Editor and the brush radial control. Any other modal action ends an
   * in-flight value drag. */
  Scene *scene = CTX_data_scene(C);
  if (event->type == EVT_MODAL_MAP && scene && scene->toolsettings &&
      scene->toolsettings->sculpt)
  {
    if (ELEM(event->val,
             PAINT_SHAPE_MODAL_STROKE_WIDTH,
             PAINT_SHAPE_MODAL_STROKE_OPACITY))
    {
      const bool is_width = (event->val == PAINT_SHAPE_MODAL_STROKE_WIDTH);
      Paint &paint = scene->toolsettings->sculpt->paint;
      float start = create.style().stroke_width;
      if (!is_width) {
        if (Brush *brush = BKE_paint_brush(&paint)) {
          start = BKE_brush_alpha_get(&paint, brush);
        }
      }
      create.begin_value_drag(is_width, int2(event->mval[0], event->mval[1]), start);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->val == PAINT_SHAPE_MODAL_AXIS_X) {
      /* X swaps the shape's own Stroke / Fill colors; rebuild the style so the in-progress shape
       * uses the new pair. */
      WM_operator_name_call(
          C, "PAINT_OT_shape_colors_swap", wm::OpCallContext::ExecDefault, nullptr, nullptr);
      shape::ShapeStyle style = shape::style_from_settings(
          BKE_paint_shape_settings_get(*scene->toolsettings));
      shape::style_brush_values_from_brush(scene->toolsettings->sculpt->paint, style);
      create.set_style(style);
      ED_region_tag_redraw(state->owner_region);
      return OPERATOR_RUNNING_MODAL;
    }
    create.end_value_drag();
  }
  if (create.value_drag_active()) {
    if (event->type == EVT_FKEY && event->val == KM_RELEASE) {
      create.end_value_drag();
      return OPERATOR_RUNNING_MODAL;
    }
    if (ELEM(event->type, MOUSEMOVE, INBETWEEN_MOUSEMOVE) && scene && scene->toolsettings &&
        scene->toolsettings->sculpt)
    {
      const float value = create.update_value_drag(int2(event->mval[0], event->mval[1]));
      if (create.value_drag_is_width()) {
        BKE_paint_shape_settings_get(*scene->toolsettings).stroke_width = value;
      }
      else {
        /* The opacity itself is derived from the brush strength in one place
         * (#style_brush_values_from_brush); the drag only writes the strength. */
        Paint &paint = scene->toolsettings->sculpt->paint;
        if (Brush *brush = BKE_paint_brush(&paint)) {
          BKE_brush_alpha_set(&paint, brush, value);
        }
      }
      WM_main_add_notifier(NC_SCENE | ND_TOOLSETTINGS, scene);
      ED_region_tag_redraw(state->owner_region);
      return OPERATOR_RUNNING_MODAL;
    }
  }

  /* Positions are measured in the invoking region; events routed through another region keep
   * the last known cursor instead of jumping by that region's offset. */
  const bool in_owner_region = (region == state->owner_region);
  /* Surface mode authors in plane pixels, so the event must go through the mapping; view mode's
   * identity mapping returns the region pixels unchanged. */
  const float2 event_px = in_owner_region ? state->mapping.event_to_user(*region, *event) :
                                            create.cursor_px();

  switch (create.handle_event(*event, event_px, in_owner_region)) {
    case shape::ShapeCreateResult::Confirmed:
      return shape_apply(C, op, state);
    case shape::ShapeCreateResult::Cancelled:
      shape_state_free(C, op);
      return OPERATOR_CANCELLED;
    case shape::ShapeCreateResult::Changed:
      ED_region_tag_redraw(state->owner_region);
      return OPERATOR_RUNNING_MODAL;
    case shape::ShapeCreateResult::None:
      return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_RUNNING_MODAL;
}

static void shape_draw_cancel(bContext *C, wmOperator *op)
{
  shape_state_free(C, op);
}

/**
 * Replay the stored draw: Python (`bpy.ops.sculpt.paint_shape_draw(...)`) and Redo (F9) rebuild
 * the shape, style and frozen space from the operator properties and apply them. The modal
 * writes those properties before it calls the same #shape::shape_apply_3d path.
 */
static wmOperatorStatus shape_draw_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return OPERATOR_CANCELLED;
  }
  std::optional<shape::PaintShape> shape = shape::shape_from_op_props(op);
  if (!shape || shape->is_empty()) {
    return OPERATOR_CANCELLED;
  }
  shape::ShapeStyle style = shape::style_from_settings(
      BKE_paint_shape_settings_get(*scene->toolsettings));
  /* Redo (F9) replays the style stored with the operator; a fresh call keeps the settings. */
  if (!shape::style_from_op_props(op, style) && scene->toolsettings->sculpt != nullptr) {
    shape::style_brush_values_from_brush(scene->toolsettings->sculpt->paint, style);
  }
  shape::ShapeSpaceDesc space_desc;
  if (!shape::space_from_op_props(op, space_desc)) {
    /* No frozen view stored (a bare Python call without a draw): nothing to reproduce. */
    return OPERATOR_CANCELLED;
  }
  Object *ob = CTX_data_active_object(C);
  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const Vector<shape::PaintShape> shapes = {*shape};
  if (shape::shape_apply_3d(C, op, *ob, space_desc, shapes, style)) {
    if (ARegion *region = CTX_wm_region(C)) {
      ED_region_tag_redraw(region);
    }
    return OPERATOR_FINISHED;
  }
  return OPERATOR_CANCELLED;
}

/** \} */

}  // namespace shape_3d

/* -------------------------------------------------------------------- */
/** \name Stroke-width cursor
 *
 * The shape tools draw a stroke-width circle around the cursor, like the brush paint cursor
 * (`paint_cursor.cc`): same immediate-mode outline, smooth 1px line. It is a global paint
 * cursor (registered once from #ED_paint_cursor_start, like the paint-curve overlay cursor)
 * whose poll selects the open shape tools; the draw runs for every region redraw, including
 * during the modal drawing operator, so the width stays readable while F / Shift+F adjust it.
 * \{ */

static bool shape_cursor_poll(bContext *C)
{
  const bToolRef *tool = WM_toolsystem_ref_from_context(C);
  if (tool == nullptr) {
    return false;
  }
  const char *idname = tool->idname;
  if (!(STRPREFIX(idname, "builtin.paint_shape_line") ||
        STRPREFIX(idname, "builtin.paint_shape_polyline") ||
        STRPREFIX(idname, "builtin.paint_shape_curve")))
  {
    return false;
  }
  /* Only the main window region of the spaces the shape tools run in (not sidebars or the
   * quad-view helper regions). */
  const ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->regiontype != RGN_TYPE_WINDOW) {
    return false;
  }
  return CTX_wm_view3d(C) != nullptr || CTX_wm_space_image(C) != nullptr;
}

static void shape_cursor_draw(bContext *C,
                              const int2 &xy,
                              const float2 & /*tilt*/,
                              void * /*customdata*/)
{
  ARegion *region = CTX_wm_region(C);
  Scene *scene = CTX_data_scene(C);
  if (region == nullptr || scene == nullptr) {
    return;
  }
  const float width = BKE_paint_shape_settings_get(*scene->toolsettings).stroke_width;
  /* Region-relative position; the matrix is translated by the region origin below (the same
   * window-vs-region handling as #paint_draw_alpha_overlay in paint_cursor.cc). */
  const float rx = float(xy[0] - region->winrct.xmin);
  const float ry = float(xy[1] - region->winrct.ymin);

  float radius = width * 0.5f;
  if (CTX_wm_view3d(C) == nullptr) {
    /* Image Editor: stroke width is in reference-tile pixels, so it is converted to region
     * pixels through the View2D mapping, the same mapping the shape overlay uses. */
    SpaceImage *sima = CTX_wm_space_image(C);
    if (sima != nullptr && sima->image != nullptr) {
      int tile_size[2];
      BKE_image_get_size(sima->image, &sima->iuser, &tile_size[0], &tile_size[1]);
      if (tile_size[0] > 0) {
        float uv[2];
        ui::view2d_region_to_view(&region->v2d, rx, ry, &uv[0], &uv[1]);
        float sx, sy;
        ui::view2d_view_to_region_fl(
            &region->v2d, uv[0] + width / float(tile_size[0]), uv[1], &sx, &sy);
        radius = math::length(float2(sx - rx, sy - ry));
      }
    }
  }
  if (radius < 1.0f) {
    return;
  }

  /* Translate to region space, exactly like the brush cursor overlay; draws at (rx, ry). */
  GPU_matrix_push();
  GPU_matrix_translate_2f(region->winrct.xmin, region->winrct.ymin);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_line_width(1.0f);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  const float col[3] = {0.8f, 0.8f, 0.8f};
  immUniformColor3fvAlpha(col, 0.8f);
  imm_draw_circle_wire_2d(pos, rx, ry, radius, 80);
  GPU_line_smooth(false);
  GPU_blend(GPU_BLEND_NONE);
  immUnbindProgram();

  GPU_matrix_pop();
}

/* Ctrl insert preview: the perpendicular tick and inward chevrons of the Curve stroke method's
 * Insert Point, drawn where a Ctrl+RMB cut of the live Vector shape would land. A paint cursor
 * redraws with every mouse move, which the session overlay (no modal running) cannot. */

static bool shape_insert_cursor_poll(bContext *C)
{
  const wmWindow *win = CTX_wm_window(C);
  const ARegion *region = CTX_wm_region(C);
  if (win == nullptr || win->runtime == nullptr || win->runtime->eventstate == nullptr ||
      (win->runtime->eventstate->modifier & KM_CTRL) == 0 || region == nullptr ||
      region->regiontype != RGN_TYPE_WINDOW || CTX_wm_view3d(C) == nullptr)
  {
    return false;
  }
  const bToolRef *tool = WM_toolsystem_ref_from_context(C);
  Object *ob = CTX_data_active_object(C);
  if (tool == nullptr || ob == nullptr || !STRPREFIX(tool->idname, "builtin.paint_shape_")) {
    return false;
  }
  const shape::PaintShapeSession *session = shape::paint_shape_session_get(*ob);
  return session != nullptr && shape::paint_shape_session_tool_matches(*session, tool->idname);
}

static void shape_insert_cursor_draw(bContext *C,
                                     const int2 &xy,
                                     const float2 & /*tilt*/,
                                     void * /*customdata*/)
{
  const ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  const shape::PaintShapeSession *session = ob != nullptr ? shape::paint_shape_session_get(*ob) :
                                                            nullptr;
  if (region == nullptr || session == nullptr) {
    return;
  }
  const int2 mval(xy[0] - region->winrct.xmin, xy[1] - region->winrct.ymin);
  float2 point, tangent;
  if (!shape::paint_shape_session_insert_preview(
          *session,
          *region,
          mval,
          shape_3d::shape_hit_tolerance_px() * shape::SHAPE_INSERT_TOLERANCE_SCALE,
          point,
          tangent))
  {
    return;
  }
  tangent = math::normalize(tangent);
  const float2 perp(-tangent.y, tangent.x);
  const float scale = UI_SCALE_FAC;
  const float half_len = 14.0f * scale;
  const float arrow_inset = 6.0f * scale;
  const float arrow_len = 6.0f * scale;
  const float arrow_wing = 4.0f * scale;

  GPU_matrix_push();
  GPU_matrix_translate_2f(region->winrct.xmin, region->winrct.ymin);
  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);

  auto draw_pass = [&](const float width, const float4 &color) {
    GPU_line_width(width);
    immUniformColor4fv(color);
    immBegin(GPU_PRIM_LINES, 2 + 2 * 4);
    const float2 a = point - perp * half_len;
    const float2 b = point + perp * half_len;
    immVertex2fv(pos, a);
    immVertex2fv(pos, b);
    /* Chevrons on each side, tips pointing at the contour. */
    for (const float side : {-1.0f, 1.0f}) {
      const float2 tip = point + perp * (side * arrow_inset);
      const float2 back = tip + perp * (side * arrow_len);
      immVertex2fv(pos, tip);
      immVertex2fv(pos, back + tangent * arrow_wing);
      immVertex2fv(pos, tip);
      immVertex2fv(pos, back - tangent * arrow_wing);
    }
    immEnd();
  };
  draw_pass(3.0f, float4(0.0f, 0.0f, 0.0f, 0.5f));
  draw_pass(1.0f, float4(1.0f, 1.0f, 1.0f, 0.9f));

  GPU_line_smooth(false);
  GPU_blend(GPU_BLEND_NONE);
  immUnbindProgram();
  GPU_matrix_pop();
}

void ED_paint_shape_cursor_register()
{
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  WM_paint_cursor_activate(
      SPACE_TYPE_ANY, RGN_TYPE_WINDOW, shape_cursor_poll, shape_cursor_draw, nullptr);
  WM_paint_cursor_activate(
      SPACE_TYPE_ANY, RGN_TYPE_WINDOW, shape_insert_cursor_poll, shape_insert_cursor_draw, nullptr);
}

/** \} */

void SCULPT_OT_paint_shape_draw(wmOperatorType *ot)
{
  ot->name = "Paint Shape";
  ot->idname = "SCULPT_OT_paint_shape_draw";
  ot->description =
      "Draw a shape onto the mesh: drag for line/rectangle/ellipse, click to place points for "
      "polyline and curve patch; Shift snaps, Alt grows from the center, Space moves";

  ot->invoke = shape_3d::shape_draw_invoke;
  ot->exec = shape_3d::shape_draw_exec;
  ot->modal = shape_3d::shape_draw_modal;
  ot->cancel = shape_3d::shape_draw_cancel;
  ot->poll = sculpt_mode_poll_view3d;

  /* Mirrors #SCULPT_OT_color_gradient: the backends finalize their own undo step (the operator's
   * OPTYPE_UNDO push is a no-op in Sculpt Mode, see §7 O16), and OPTYPE_REGISTER makes the draw a
   * Redo (F9) anchor replayed by #shape_draw_exec. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  static const EnumPropertyItem type_items[] = {
      {-1, "DEFAULT", 0, "Default", "Use the type from the tool settings"},
      {PAINT_SHAPE_LINE, "LINE", 0, "Line", "Straight line"},
      {PAINT_SHAPE_POLYLINE, "POLYLINE", 0, "Polyline", "Chain of straight segments"},
      {PAINT_SHAPE_RECT, "RECTANGLE", 0, "Rectangle", "Rectangle with rounded corners"},
      {PAINT_SHAPE_ELLIPSE, "ELLIPSE", 0, "Ellipse", "Ellipse"},
      {PAINT_SHAPE_CURVE, "CURVE", 0, "Curve Patch", "Bézier curve patch"},
      {PAINT_SHAPE_POLYGON, "POLYGON", 0, "Polygon", "Regular polygon"},
      {PAINT_SHAPE_STAR, "STAR", 0, "Star", "Star polygon"},
      {PAINT_SHAPE_ARC, "ARC", 0, "Arc", "Elliptical arc"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  PropertyRNA *prop = RNA_def_property(ot->srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, type_items);
  RNA_def_property_enum_default(prop, -1);
  RNA_def_property_ui_text(prop, "Type", "Shape to draw");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  /* Placement projection of Pixel mode: View (default) or Surface. Vector ignores it (always
   * Surface). Not hidden, so the tool header / N-panel can show and edit it through
   * `tool.operator_properties("sculpt.paint_shape_draw")`; the per-tool value is applied on
   * invocation like any other tool operator property. The resolved space is still stored
   * separately in the op-props for F9. `PROP_SKIP_SAVE` keeps it out of the operator's global
   * last-used state (the tool's own storage is authoritative). */
  static const EnumPropertyItem space_items[] = {
      {0, "VIEW", 0, "View", "Project the shape through the viewport"},
      {1, "SURFACE", 0, "Surface", "Anchor the shape to the surface under the first click"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  prop = RNA_def_property(ot->srna, "space", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, space_items);
  RNA_def_property_enum_default(prop, 0);
  RNA_def_property_ui_text(prop, "Projection", "How the drawn shape is placed");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  /* Serialization properties for exec / Redo (F9): the drawn shape, its style and the frozen
   * view it was projected through. */
  shape::shape_op_properties_register(ot);
  shape::style_op_properties_register(ot);
  shape::space_op_properties_register(ot);
}

}  // namespace blender::ed::sculpt_paint
