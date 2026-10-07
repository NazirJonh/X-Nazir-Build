/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared cage gizmo core; see `paint_shape_gizmo.hh`.
 */

#include "paint_shape_gizmo.hh"

#include "MEM_guardedalloc.h"

#include "BLI_math_base.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"

#include "DNA_space_types.h"
#include "DNA_userdef_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_gizmo_library.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../shapes/paint_shape.hh"
#include "../shapes/paint_shape_edit.hh"
#include "../shapes/paint_vector_editor.hh"

namespace blender::ed::sculpt_paint::shape {

struct PaintShapeTransformGizmoGroup {
  const PaintShapeGizmoAdapter *adapter = nullptr;
  wmGizmo *gz_cage = nullptr;
  wmGizmo *gz_anchor = nullptr;
  /** Tracks #WM_gizmomap_get_modal so a finished native tweak clears the drag state. */
  bool was_modal_tweak = false;
  /** True while one of our own handles is being dragged, so #refresh keeps us visible. */
  bool drag_active = false;
  PaintShapeGizmoHandle handle = PaintShapeGizmoHandle::None;
};

static PaintShapeTransformGizmoGroup *paint_shape_gizmo_group(wmGizmoGroup *gzgroup)
{
  return static_cast<PaintShapeTransformGizmoGroup *>(gzgroup->customdata);
}

/**
 * Cage matrix and origin-anchor placement. The cage follows the shape's center, half size and
 * rotation; the pivot is #shape_effective_origin. Returns \a r_anchor_space / \a r_anchor for the
 * move gizmo: a frame plus a world/region location, deliberately separate from the cage's frame
 * because the 3D cage frame carries the plane-pixel scale while the anchor must stay unit-scaled
 * so the gizmo can size its ring to a constant screen radius.
 *
 * 2D (Image Editor): identity spaces and everything in region pixels (the affine cage built from
 * the projected corners).
 * 3D (Viewport): the cage frame is the tangent plane's world matrix and the cage offset stays in
 * tangent-plane pixels, so perspective bends the cage like the shape (a trapezoid, not the
 * parallelogram an affine cage would give).
 */
static bool paint_shape_gizmo_matrices(const bContext *C,
                                       const PaintShapeGizmoAdapter &adapter,
                                       const ARegion *region,
                                       const PaintShape &shape,
                                       float4x4 &r_space,
                                       float4x4 &r_offset,
                                       float4x4 &r_anchor_space,
                                       float r_anchor[3])
{
  if (region == nullptr) {
    return false;
  }
  const float2 half = shape.half_size;
  if (half.x < 1e-6f || half.y < 1e-6f) {
    return false;
  }
  const float c = math::cos(shape.rotation);
  const float s = math::sin(shape.rotation);

  if (adapter.is_3d) {
    if (adapter.space_matrix == nullptr || !adapter.space_matrix(C, shape, r_space)) {
      return false;
    }
    /* Everything in tangent-plane pixels; #r_space maps them to world. The edge vectors are the
     * shape's own half-size/rotation, so the cage sits exactly on the authored rectangle. */
    const float2 e0(2.0f * half.x * c, 2.0f * half.x * s);
    const float2 e1(-2.0f * half.y * s, 2.0f * half.y * c);
    r_offset = float4x4::identity();
    r_offset[0][0] = e0.x;
    r_offset[0][1] = e0.y;
    r_offset[1][0] = e1.x;
    r_offset[1][1] = e1.y;
    r_offset[3][0] = shape.center.x;
    r_offset[3][1] = shape.center.y;

    /* Anchor: same plane orientation but unit scale (drop the plane-pixel scale), with the pivot's
     * world position baked into the frame so the ring keeps a constant screen radius. The frame is
     * orthonormalized so a skewed object matrix cannot shear the ring. */
    const float3 ex = math::normalize(float3(r_space[0].x, r_space[0].y, r_space[0].z));
    const float3 y_raw = float3(r_space[1].x, r_space[1].y, r_space[1].z);
    const float3 ey = math::normalize(y_raw - ex * math::dot(ex, y_raw));
    const float3 ez = math::normalize(math::cross(ex, ey));
    const float2 origin = shape_effective_origin(shape);
    const float3 origin_world = math::transform_point(r_space, float3(origin.x, origin.y, 0.0f));
    r_anchor_space = float4x4::identity();
    r_anchor_space[0] = float4(ex, 0.0f);
    r_anchor_space[1] = float4(ey, 0.0f);
    r_anchor_space[2] = float4(ez, 0.0f);
    r_anchor_space[3] = float4(origin_world, 1.0f);
    r_anchor[0] = 0.0f;
    r_anchor[1] = 0.0f;
    r_anchor[2] = 0.0f;
    return true;
  }

  auto corner_region = [&](const float lx, const float ly) {
    const float2 local(lx * c - ly * s, lx * s + ly * c);
    return adapter.shape_px_to_region(C, *region, shape, shape.center + local);
  };
  const float2 bl = corner_region(-half.x, -half.y);
  const float2 br = corner_region(half.x, -half.y);
  const float2 tr = corner_region(half.x, half.y);
  const float2 tl = corner_region(-half.x, half.y);

  const float2 e0 = br - bl; /* Cage +X: bottom-left -> bottom-right. */
  const float2 e1 = tl - bl; /* Cage +Y: bottom-left -> top-left. */
  if (math::length_squared(e0) < 1e-8f || math::length_squared(e1) < 1e-8f) {
    return false;
  }
  const float2 center = 0.25f * (bl + br + tr + tl);

  r_space = float4x4::identity();
  r_anchor_space = float4x4::identity();
  r_offset = float4x4::identity();
  r_offset[0][0] = e0.x;
  r_offset[0][1] = e0.y;
  r_offset[1][0] = e1.x;
  r_offset[1][1] = e1.y;
  r_offset[3][0] = center.x;
  r_offset[3][1] = center.y;

  const float2 anchor = adapter.shape_px_to_region(C, *region, shape, shape_effective_origin(shape));
  r_anchor[0] = anchor.x;
  r_anchor[1] = anchor.y;
  r_anchor[2] = 0.0f;
  return true;
}

static PaintShapeGizmoHandle paint_shape_gizmo_part_to_handle(const int cage_part)
{
  switch (cage_part) {
    case ED_GIZMO_CAGE2D_PART_TRANSLATE:
      return PaintShapeGizmoHandle::Move;
    case ED_GIZMO_CAGE2D_PART_ROTATE:
      return PaintShapeGizmoHandle::Rotate;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X:
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X:
      return PaintShapeGizmoHandle::ScaleLocalX;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_Y:
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_Y:
      return PaintShapeGizmoHandle::ScaleLocalY;
    case ED_GIZMO_CAGE2D_PART_SCALE:
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MIN_Y:
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MAX_Y:
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MIN_Y:
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MAX_Y:
      return PaintShapeGizmoHandle::ScaleUniform;
    default:
      return PaintShapeGizmoHandle::None;
  }
}

/**
 * Finish (or cancel) the in-flight gesture, mirror the shape into the settings and re-stamp.
 * \a restamp is false when settling from the refresh callback (a redraw): the update loop already
 * composited the final pose, and re-entering the compositor while drawing is unsafe. A cancel
 * restores the pre-gesture pose, so it always needs the re-stamp.
 */
static void paint_shape_gizmo_end_drag(PaintShapeTransformGizmoGroup *ggd,
                                       VectorEditHost *host,
                                       bContext *C,
                                       const bool cancel,
                                       const bool restamp)
{
  if (host != nullptr) {
    switch (ggd->handle) {
      case PaintShapeGizmoHandle::Move:
        cancel ? host->edit().drag_cancel() : host->edit().drag_end();
        break;
      case PaintShapeGizmoHandle::Rotate:
      case PaintShapeGizmoHandle::ScaleUniform:
      case PaintShapeGizmoHandle::ScaleFree:
      case PaintShapeGizmoHandle::ScaleLocalX:
      case PaintShapeGizmoHandle::ScaleLocalY:
        cancel ? host->edit().transform_cancel() : host->edit().transform_end();
        break;
      case PaintShapeGizmoHandle::Origin:
        cancel ? host->edit().origin_drag_cancel() : host->edit().origin_drag_end();
        break;
      case PaintShapeGizmoHandle::None:
        break;
    }
    if (restamp && (cancel || ggd->handle != PaintShapeGizmoHandle::Origin)) {
      host->restamp(C);
    }
    host->sync_settings_from_active();
    /* Force the final preview tag past the throttle (a no-op for the flat 2D canvas). */
    host->interaction_end(C);
    host->redraw(C);
  }
  ggd->handle = PaintShapeGizmoHandle::None;
  ggd->drag_active = false;
}

static wmOperatorStatus paint_shape_gizmo_modal(bContext *C,
                                                wmGizmo *gz,
                                                const wmEvent *event,
                                                eWM_GizmoFlagTweak /*tweak_flag*/)
{
  PaintShapeTransformGizmoGroup *ggd = paint_shape_gizmo_group(gz->parent_gzgroup);
  ARegion *region = CTX_wm_region(C);
  if (ggd == nullptr || ggd->adapter == nullptr || region == nullptr) {
    return OPERATOR_FINISHED;
  }
  std::unique_ptr<VectorEditHost> host = ggd->adapter->host_create(*C);
  if (host == nullptr) {
    return OPERATOR_FINISHED;
  }
  const PaintShape *shape = host->edit().active();
  if (shape == nullptr) {
    return OPERATOR_FINISHED;
  }

  if ((event->type == EVT_ESCKEY) || (event->type == RIGHTMOUSE && event->val == KM_PRESS)) {
    paint_shape_gizmo_end_drag(ggd, host.get(), C, true, true);
    return OPERATOR_CANCELLED;
  }
  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    paint_shape_gizmo_end_drag(ggd, host.get(), C, false, true);
    return OPERATOR_FINISHED;
  }
  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  float2 p;
  if (!ggd->adapter->event_to_shape_px(C, *region, *shape, *event, p)) {
    return OPERATOR_RUNNING_MODAL;
  }
  switch (ggd->handle) {
    case PaintShapeGizmoHandle::Move:
      if (host->edit().drag_update(p)) {
        host->restamp(C);
      }
      break;
    case PaintShapeGizmoHandle::Rotate:
    case PaintShapeGizmoHandle::ScaleUniform:
    case PaintShapeGizmoHandle::ScaleFree:
    case PaintShapeGizmoHandle::ScaleLocalX:
    case PaintShapeGizmoHandle::ScaleLocalY:
      if (host->edit().transform_update(p)) {
        host->restamp(C);
      }
      break;
    case PaintShapeGizmoHandle::Origin:
      if (host->edit().origin_drag_update(p)) {
        host->redraw(C);
      }
      break;
    case PaintShapeGizmoHandle::None:
      break;
  }
  return OPERATOR_RUNNING_MODAL;
}

void paint_shape_gizmo_group_invoke_prepare(const bContext *C,
                                            wmGizmoGroup *gzgroup,
                                            wmGizmo *gz,
                                            const wmEvent *event)
{
  PaintShapeTransformGizmoGroup *ggd = paint_shape_gizmo_group(gzgroup);
  ARegion *region = CTX_wm_region(C);
  if (ggd == nullptr || ggd->adapter == nullptr || region == nullptr || event == nullptr) {
    return;
  }
  std::unique_ptr<VectorEditHost> host = ggd->adapter->host_create(const_cast<bContext &>(*C));
  if (host == nullptr) {
    return;
  }
  const PaintShape *shape = host->edit().active();
  if (shape == nullptr) {
    return;
  }
  float2 p;
  if (!ggd->adapter->event_to_shape_px(C, *region, *shape, *event, p)) {
    return;
  }

  PaintShapeGizmoHandle handle = PaintShapeGizmoHandle::None;
  if (gz == ggd->gz_anchor) {
    if (host->edit().origin_drag_begin(p)) {
      handle = PaintShapeGizmoHandle::Origin;
    }
  }
  else if (gz == ggd->gz_cage) {
    handle = paint_shape_gizmo_part_to_handle(gz->highlight_part);
    if (handle == PaintShapeGizmoHandle::ScaleUniform && (event->modifier & KM_SHIFT)) {
      handle = PaintShapeGizmoHandle::ScaleFree;
    }
    switch (handle) {
      case PaintShapeGizmoHandle::Move:
        if (!host->edit().move_active_begin(p)) {
          handle = PaintShapeGizmoHandle::None;
        }
        break;
      case PaintShapeGizmoHandle::Rotate:
        if (!host->edit().transform_begin(ShapeTransformMode::Rotate, p, host->style())) {
          handle = PaintShapeGizmoHandle::None;
        }
        break;
      case PaintShapeGizmoHandle::ScaleUniform:
        if (!host->edit().transform_begin(ShapeTransformMode::Scale, p, host->style())) {
          handle = PaintShapeGizmoHandle::None;
        }
        break;
      case PaintShapeGizmoHandle::ScaleFree:
        if (!host->edit().transform_begin(ShapeTransformMode::ScaleFree, p, host->style())) {
          handle = PaintShapeGizmoHandle::None;
        }
        break;
      case PaintShapeGizmoHandle::ScaleLocalX:
        if (!host->edit().transform_begin(
                ShapeTransformMode::ScaleFree, p, host->style(), ShapeScaleAxis::X))
        {
          handle = PaintShapeGizmoHandle::None;
        }
        break;
      case PaintShapeGizmoHandle::ScaleLocalY:
        if (!host->edit().transform_begin(
                ShapeTransformMode::ScaleFree, p, host->style(), ShapeScaleAxis::Y))
        {
          handle = PaintShapeGizmoHandle::None;
        }
        break;
      default:
        break;
    }
  }

  ggd->handle = handle;
  ggd->drag_active = (handle != PaintShapeGizmoHandle::None);
}

bool paint_shape_gizmo_hit(const bContext *C,
                           const int mval[2],
                           const PaintShapeGizmoAdapter &adapter)
{
  ARegion *region = CTX_wm_region(C);
  if (region == nullptr || region->runtime == nullptr) {
    return false;
  }
  wmGizmoMap *gzmap = region->runtime->gizmo_map;
  if (gzmap == nullptr) {
    return false;
  }
  wmGizmoGroup *gzgroup = WM_gizmomap_group_find(gzmap, adapter.group_idname);
  if (gzgroup == nullptr) {
    return false;
  }
  PaintShapeTransformGizmoGroup *ggd = paint_shape_gizmo_group(gzgroup);
  if (ggd == nullptr || adapter.target(C) == nullptr) {
    return false;
  }
  bContext *ctx = const_cast<bContext *>(C);
  if (ggd->gz_cage != nullptr && ggd->gz_cage->type->test_select != nullptr &&
      ggd->gz_cage->type->test_select(ctx, ggd->gz_cage, mval) != -1)
  {
    return true;
  }
  if (ggd->gz_anchor != nullptr && ggd->gz_anchor->type->test_select != nullptr &&
      ggd->gz_anchor->type->test_select(ctx, ggd->gz_anchor, mval) != -1)
  {
    return true;
  }
  return false;
}

bool paint_shape_gizmo_group_poll(const bContext *C, const PaintShapeGizmoAdapter &adapter)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }
  return adapter.poll(C);
}

bool paint_shape_gizmo_group_drag_active(const wmGizmoGroup *gzgroup)
{
  const PaintShapeTransformGizmoGroup *ggd = paint_shape_gizmo_group(
      const_cast<wmGizmoGroup *>(gzgroup));
  return ggd != nullptr && ggd->drag_active;
}

void paint_shape_gizmo_group_setup(const bContext * /*C*/,
                                   wmGizmoGroup *gzgroup,
                                   const PaintShapeGizmoAdapter *adapter)
{
  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_2d", true);
  const wmGizmoType *gzt_move3d = WM_gizmotype_find("GIZMO_GT_move_3d", true);

  PaintShapeTransformGizmoGroup *ggd = MEM_new<PaintShapeTransformGizmoGroup>(__func__);
  ggd->adapter = adapter;
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<PaintShapeTransformGizmoGroup *>(customdata));
  };

  /* Anchor before cage so the pivot hit wins over the interior move region. */
  ggd->gz_anchor = WM_gizmo_new_ptr(gzt_move3d, gzgroup, nullptr);
  WM_gizmo_set_fn_custom_modal(ggd->gz_anchor, paint_shape_gizmo_modal);
  RNA_enum_set(ggd->gz_anchor->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_RING_2D);
  ggd->gz_anchor->flag |= WM_GIZMO_DRAW_MODAL | WM_GIZMO_DRAW_NO_SCALE;
  float anchor_color[4] = {0.1f, 0.6f, 1.0f, 1.0f};
  float anchor_color_hi[4] = {0.3f, 0.8f, 1.0f, 1.0f};
  WM_gizmo_set_color(ggd->gz_anchor, anchor_color);
  WM_gizmo_set_color_highlight(ggd->gz_anchor, anchor_color_hi);
  WM_gizmo_set_scale(ggd->gz_anchor, 0.15f);

  ggd->gz_cage = WM_gizmo_new_ptr(gzt_cage, gzgroup, nullptr);
  RNA_enum_set(ggd->gz_cage->ptr, "draw_style", ED_GIZMO_CAGE2D_STYLE_BOX_TRANSFORM);
  RNA_enum_set(ggd->gz_cage->ptr,
               "transform",
               ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_SCALE |
                   ED_GIZMO_CAGE_XFORM_FLAG_ROTATE);
  RNA_enum_set(ggd->gz_cage->ptr,
               "draw_options",
               ED_GIZMO_CAGE_DRAW_FLAG_XFORM_CENTER_HANDLE |
                   ED_GIZMO_CAGE_DRAW_FLAG_XFORM_CENTER_HANDLE_PLUS |
                   ED_GIZMO_CAGE_DRAW_FLAG_CORNER_HANDLES | ED_GIZMO_CAGE_DRAW_FLAG_ALL_HANDLES |
                   ED_GIZMO_CAGE_DRAW_FLAG_XFORM_INTERIOR_TRANSLATE);
  WM_gizmo_set_fn_custom_modal(ggd->gz_cage, paint_shape_gizmo_modal);
  ggd->gz_cage->flag |= WM_GIZMO_REFRESH_CURSOR_ON_MODAL;

  float cage_color[4] = {1.0f, 0.85f, 0.0f, 0.9f};
  float cage_color_hi[4] = {1.0f, 1.0f, 0.3f, 1.0f};
  WM_gizmo_set_color(ggd->gz_cage, cage_color);
  WM_gizmo_set_color_highlight(ggd->gz_cage, cage_color_hi);
  WM_gizmo_set_line_width(ggd->gz_cage, 1.5f);
}

void paint_shape_gizmo_group_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  PaintShapeTransformGizmoGroup *ggd = paint_shape_gizmo_group(gzgroup);
  if (ggd == nullptr || ggd->adapter == nullptr) {
    return;
  }
  const bool target = ggd->adapter->target(C) != nullptr;
  const ShapeEditSession *edit = ggd->adapter->edit(C);
  /* Hide during the operator's own G/R/S gestures, but stay shown during our handle drags. */
  const bool gesture = edit != nullptr && edit->gesture_active();
  const bool show = target && (!gesture || ggd->drag_active);

  if (show) {
    ggd->gz_cage->flag &= ~WM_GIZMO_HIDDEN;
    ggd->gz_anchor->flag &= ~WM_GIZMO_HIDDEN;
  }
  else {
    ggd->gz_cage->flag |= WM_GIZMO_HIDDEN;
    ggd->gz_anchor->flag |= WM_GIZMO_HIDDEN;
  }

  /* GIZMOGROUP_OT_gizmo_tweak finishes on button release before custom_modal runs, so the release
   * handler never clears the drag. Detect the native modal ending here and settle the gesture. */
  ARegion *region = CTX_wm_region(C);
  wmGizmoMap *gzmap = (region && region->runtime) ? region->runtime->gizmo_map : nullptr;
  wmGizmo *modal_gz = gzmap ? WM_gizmomap_get_modal(gzmap) : nullptr;
  const bool is_our_modal = modal_gz != nullptr &&
                            (modal_gz == ggd->gz_cage || modal_gz == ggd->gz_anchor);
  /* No "was modal on an earlier redraw" requirement: a quick click (or a slow redraw, as with a
   * textured shape) can press and release between two refreshes, and the drag would then stay
   * armed forever, freezing every handle. The handle is armed and the modal started within one
   * event, so a refresh that finds a drag without our modal always means it is over. */
  if (!is_our_modal && ggd->drag_active) {
    std::unique_ptr<VectorEditHost> host = ggd->adapter->host_create(const_cast<bContext &>(*C));
    if (host != nullptr) {
      paint_shape_gizmo_end_drag(ggd, host.get(), const_cast<bContext *>(C), false, false);
    }
    else {
      ggd->handle = PaintShapeGizmoHandle::None;
      ggd->drag_active = false;
    }
  }
  ggd->was_modal_tweak = is_our_modal;
}

void paint_shape_gizmo_group_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  PaintShapeTransformGizmoGroup *ggd = paint_shape_gizmo_group(gzgroup);
  if (ggd == nullptr || ggd->adapter == nullptr) {
    return;
  }
  const PaintShape *shape = ggd->adapter->target(C);
  ARegion *region = CTX_wm_region(C);
  if (shape == nullptr || region == nullptr) {
    return;
  }

  float4x4 matrix_space;
  float4x4 matrix_offset;
  float4x4 anchor_space;
  float anchor[3];
  if (!paint_shape_gizmo_matrices(
          C, *ggd->adapter, region, *shape, matrix_space, matrix_offset, anchor_space, anchor))
  {
    return;
  }

  copy_m4_m4(ggd->gz_cage->matrix_space, matrix_space.ptr());
  unit_m4(ggd->gz_cage->matrix_basis);
  copy_m4_m4(ggd->gz_cage->matrix_offset, matrix_offset.ptr());

  copy_m4_m4(ggd->gz_anchor->matrix_space, anchor_space.ptr());
  unit_m4(ggd->gz_anchor->matrix_basis);
  unit_m4(ggd->gz_anchor->matrix_offset);
  WM_gizmo_set_matrix_location(ggd->gz_anchor, anchor);
}

}  // namespace blender::ed::sculpt_paint::shape
