/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Image Editor gizmo for the UV mapping of the shown layer row: a cage2d over the map's visible
 * footprint plus a move_3d anchor for the pivot. Move, scale and rotate write the row's mapping
 * value-only while dragging, with one undo step per gesture. A Fill is made with its mapping on, so
 * the cage is there before the first texture, over the identity footprint; the values take effect
 * when a map is slotted.
 */

#include <algorithm>
#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "DNA_material_types.h"
#include "DNA_space_types.h"
#include "DNA_userdef_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_gizmo_library.hh"
#include "ED_image.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "UI_resources.hh"

#include "RNA_access.hh"

#include "UI_view2d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "uv_mapping_math.hh"
#include "uv_mapping_target.hh"

namespace blender {

namespace {

using namespace ed::sculpt_paint::uv_mapping;

/** Radius of the scale-lock buttons, in UI pixels. */
constexpr float LOCK_BUTTON_RADIUS = 14.0f;
/** Distance from the cage's top-right corner to the scale-lock buttons' center, in UI pixels. */
constexpr float LOCK_BUTTON_OFFSET = 22.0f;
/** Border width of the decorative lock outline, in pixels. */
constexpr float LOCK_OUTLINE_WIDTH = 1.5f;

enum class MappingHandle {
  None,
  Move,
  Rotate,
  Corner00,
  Corner10,
  Corner11,
  Corner01,
  EdgeMinX,
  EdgeMaxX,
  EdgeMinY,
  EdgeMaxY,
  Anchor,
};

struct LayerMappingGizmoGroup {
  wmGizmo *gz_cage = nullptr;
  wmGizmo *gz_anchor = nullptr;
  /** Scale-lock toggle: the locked icon while the axes move together, else the unlocked one. */
  wmGizmo *gz_lock_locked = nullptr;
  wmGizmo *gz_lock_unlocked = nullptr;
  /** Decorative outline drawn over whichever lock button is visible. */
  wmGizmo *gz_lock_outline = nullptr;
  bool drag_active = false;
  MappingHandle handle = MappingHandle::None;
  float2 start_mouse_uv = float2(0.0f);
  float2 drag_pivot_uv = float2(0.5f);
  UvMappingXform start_xform;
  bool start_scale_locked = false;

  Material *session_material = nullptr;
  bUUID session_marker = {};

  /**
   * A pivot placed by the anchor handle; used by the next rotate/scale drags. Kept in map
   * coordinates, so it stays on the same point of the map while the footprint is moved, scaled or
   * rotated, the way the Transform Selection anchor follows its fragment.
   */
  bool custom_pivot = false;
  float2 custom_pivot_map = float2(0.5f);
  bUUID custom_pivot_marker = {};
};

/** The cage's corners (bottom-left, bottom-right, top-right, top-left) and centre in region px. */
struct CageScreenGeometry {
  float2 corners[4];
  float2 center;
  bool valid = false;
};

LayerMappingGizmoGroup *layer_mapping_gizmo_group(wmGizmoGroup *gzgroup)
{
  return static_cast<LayerMappingGizmoGroup *>(gzgroup->customdata);
}

float2 canvas_uv_from_event(const ARegion &region, const wmEvent &event)
{
  float2 uv;
  ui::view2d_region_to_view(
      &region.v2d, float(event.mval[0]), float(event.mval[1]), &uv.x, &uv.y);
  return uv;
}

float2 region_from_canvas_uv(const ARegion &region, const float2 &uv)
{
  float2 region_px;
  ui::view2d_view_to_region_fl(&region.v2d, uv.x, uv.y, &region_px.x, &region_px.y);
  return region_px;
}

MappingHandle handle_from_cage_part(const int cage_part)
{
  switch (cage_part) {
    case ED_GIZMO_CAGE2D_PART_TRANSLATE:
      return MappingHandle::Move;
    case ED_GIZMO_CAGE2D_PART_ROTATE:
      return MappingHandle::Rotate;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MIN_Y:
      return MappingHandle::Corner00;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MIN_Y:
      return MappingHandle::Corner10;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X_MAX_Y:
      return MappingHandle::Corner11;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X_MAX_Y:
      return MappingHandle::Corner01;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_X:
      return MappingHandle::EdgeMinX;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_X:
      return MappingHandle::EdgeMaxX;
    case ED_GIZMO_CAGE2D_PART_SCALE_MIN_Y:
      return MappingHandle::EdgeMinY;
    case ED_GIZMO_CAGE2D_PART_SCALE_MAX_Y:
      return MappingHandle::EdgeMaxY;
    default:
      return MappingHandle::None;
  }
}

bool handle_is_scale(const MappingHandle handle)
{
  return handle >= MappingHandle::Corner00 && handle <= MappingHandle::EdgeMaxY;
}

UvMappingScaleAxes scale_axes(const MappingHandle handle)
{
  UvMappingScaleAxes axes;
  switch (handle) {
    case MappingHandle::Corner00:
    case MappingHandle::Corner10:
    case MappingHandle::Corner11:
    case MappingHandle::Corner01:
      axes.x = true;
      axes.y = true;
      break;
    case MappingHandle::EdgeMinX:
    case MappingHandle::EdgeMaxX:
      axes.x = true;
      break;
    case MappingHandle::EdgeMinY:
    case MappingHandle::EdgeMaxY:
      axes.y = true;
      break;
    default:
      break;
  }
  return axes;
}

/** The anchor's pivot: the one the user placed, else the centre of the mapped tile. */
float2 pivot_for_handle(const LayerMappingGizmoGroup &ggd, const UvMappingXform &xform)
{
  if (ggd.custom_pivot && BLI_uuid_equal(ggd.custom_pivot_marker, ggd.session_marker)) {
    return uv_mapping_footprint_point(xform, ggd.custom_pivot_map);
  }
  return uv_mapping_footprint_center(xform);
}

/**
 * The cage as drawn: the mapped tile scaled about its centre in UV, converted to region pixels,
 * then rotated in screen space. Doing the rotation on screen keeps the 90 degree corners under a
 * non-uniform scale, an anisotropic view and the editor's canvas rotation. Shared by the drawing
 * and the anchor's snapping, so the anchor snaps to what the user sees.
 */
CageScreenGeometry cage_screen_geometry(const ARegion &region, const UvMappingXform &xform)
{
  CageScreenGeometry geo;
  const float2 center_uv = uv_mapping_footprint_center(xform);
  geo.center = region_from_canvas_uv(region, center_uv);

  const float half_x = 0.5f / std::max(std::fabs(xform.scale.x), 1e-3f);
  const float half_y = 0.5f / std::max(std::fabs(xform.scale.y), 1e-3f);
  const float2 axis_x = region_from_canvas_uv(region, center_uv + float2(half_x, 0.0f)) -
                        geo.center;
  const float2 axis_y = region_from_canvas_uv(region, center_uv + float2(0.0f, half_y)) -
                        geo.center;

  /* Content turns by -rotation while the stored mapping rotation grows. */
  const float c = std::cos(xform.rotation);
  const float s = -std::sin(xform.rotation);
  auto rotate_screen = [&](const float2 &v) {
    return geo.center + float2(c * v.x - s * v.y, s * v.x + c * v.y);
  };

  geo.corners[0] = rotate_screen(-axis_x - axis_y);
  geo.corners[1] = rotate_screen(axis_x - axis_y);
  geo.corners[2] = rotate_screen(axis_x + axis_y);
  geo.corners[3] = rotate_screen(-axis_x + axis_y);
  geo.valid = math::length_squared(geo.corners[1] - geo.corners[0]) >= 1e-8f &&
              math::length_squared(geo.corners[3] - geo.corners[0]) >= 1e-8f;
  return geo;
}

/**
 * Where the anchor lands for a mouse at \a mouse_px: it snaps to a corner, an edge midpoint or the
 * centre of the cage within a few pixels, as the Transform Selection anchor does. Returns region
 * pixels.
 */
float2 anchor_snap_px(const CageScreenGeometry &geo, const float2 &mouse_px)
{
  if (!geo.valid) {
    return mouse_px;
  }
  const float2 points[9] = {geo.corners[0],
                            geo.corners[1],
                            geo.corners[2],
                            geo.corners[3],
                            (geo.corners[0] + geo.corners[1]) * 0.5f,
                            (geo.corners[1] + geo.corners[2]) * 0.5f,
                            (geo.corners[2] + geo.corners[3]) * 0.5f,
                            (geo.corners[3] + geo.corners[0]) * 0.5f,
                            geo.center};
  float best = 12.0f * UI_SCALE_FAC;
  float2 result = mouse_px;
  for (const float2 &point : points) {
    const float dist = math::distance(point, mouse_px);
    if (dist < best) {
      best = dist;
      result = point;
    }
  }
  return result;
}

/** Move the anchor to the canvas point under \a mouse (snapped), keeping it on the same map point. */
void anchor_place(LayerMappingGizmoGroup &ggd,
                  const ARegion &region,
                  const wmEvent &event)
{
  const CageScreenGeometry geo = cage_screen_geometry(region, ggd.start_xform);
  const float2 mouse_px = float2(float(event.mval[0]), float(event.mval[1]));
  const float2 target_px = anchor_snap_px(geo, mouse_px);
  float2 target_uv;
  ui::view2d_region_to_view(&region.v2d, target_px.x, target_px.y, &target_uv.x, &target_uv.y);

  ggd.custom_pivot = true;
  ggd.custom_pivot_marker = ggd.session_marker;
  ggd.custom_pivot_map = uv_mapping_map_point(ggd.start_xform, target_uv);
  ggd.drag_pivot_uv = target_uv;
}

void update_status_text(bContext *C, const UvMappingXform &xform)
{
  ScrArea *area = CTX_wm_area(C);
  if (area == nullptr) {
    return;
  }
  char text[256];
  BLI_snprintf(text,
               sizeof(text),
               "Offset %.4f  %.4f    Scale %.3f  %.3f    Rotation %.1f deg",
               xform.offset.x,
               xform.offset.y,
               xform.scale.x,
               xform.scale.y,
               xform.rotation * (180.0f / 3.14159265358979323846f));
  ED_area_status_text(area, text);
}

void clear_status_text(bContext *C)
{
  if (ScrArea *area = CTX_wm_area(C)) {
    ED_area_status_text(area, nullptr);
  }
}

void notify_mapping_changed(bContext *C, Material *ma, ARegion *region)
{
  if (ma != nullptr) {
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &ma->id);
  }
  if (region != nullptr) {
    ED_region_tag_redraw(region);
  }
}

void reset_drag(LayerMappingGizmoGroup &ggd)
{
  ggd.drag_active = false;
  ggd.handle = MappingHandle::None;
  ggd.session_material = nullptr;
  ggd.session_marker = {};
  ggd.start_scale_locked = false;
}

/**
 * Finish the gesture. \a commit false restores the snapshot through the value-only setters and
 * pushes no undo step. A gesture that ends without a resolvable row writes nothing.
 */
void finish_drag(bContext *C, LayerMappingGizmoGroup &ggd, const bool commit)
{
  PaintLayerRowMappingTarget target;
  const bool resolved = target.resolve(*C);
  const bool same_target = resolved && target.material() == ggd.session_material &&
                           BLI_uuid_equal(target.marker(), ggd.session_marker);

  Material *material = same_target ? target.material() : ggd.session_material;
  ARegion *region = CTX_wm_region(C);

  if (same_target) {
    if (commit) {
      UvMappingXform current;
      if (target.read(current)) {
        const UvMappingXform &start = ggd.start_xform;
        const bool changed = math::distance_squared(current.offset, start.offset) > 1e-12f ||
                             math::distance_squared(current.scale, start.scale) > 1e-12f ||
                             std::fabs(current.rotation - start.rotation) > 1e-6f;
        if (changed) {
          ED_undo_push(C, target.undo_name());
        }
      }
    }
    else {
      target.write(ggd.start_xform);
    }
  }

  clear_status_text(C);
  notify_mapping_changed(C, material, region);
  reset_drag(ggd);
}

void begin_drag(bContext *C,
                LayerMappingGizmoGroup &ggd,
                const wmEvent *event,
                const MappingHandle handle)
{
  PaintLayerRowMappingTarget target;
  ARegion *region = CTX_wm_region(C);
  if (!target.resolve(*C) || region == nullptr || event == nullptr) {
    return;
  }

  UvMappingXform xform;
  if (!target.read(xform)) {
    return;
  }

  ggd.session_material = target.material();
  ggd.session_marker = target.marker();
  ggd.start_xform = xform;
  ggd.start_scale_locked = target.scale_locked();
  ggd.handle = handle;
  ggd.start_mouse_uv = canvas_uv_from_event(*region, *event);

  /* The anchor keeps its place on a press and follows the mouse once it moves, so a plain click on
   * it changes nothing. */
  ggd.drag_pivot_uv = pivot_for_handle(ggd, xform);

  ggd.drag_active = true;
  update_status_text(C, xform);
}

void apply_drag(bContext *C,
                LayerMappingGizmoGroup &ggd,
                const wmEvent *event,
                const float2 &mouse_uv)
{
  PaintLayerRowMappingTarget target;
  if (!target.resolve(*C) || target.material() != ggd.session_material ||
      !BLI_uuid_equal(target.marker(), ggd.session_marker))
  {
    /* The row or the editor's material changed mid-drag: stop without another write. */
    finish_drag(C, ggd, false);
    return;
  }

  const float precision = (event->modifier & KM_SHIFT) ? 0.1f : 1.0f;
  UvMappingXform next = ggd.start_xform;

  switch (ggd.handle) {
    case MappingHandle::Move: {
      const float2 delta = (mouse_uv - ggd.start_mouse_uv) * precision;
      next = uv_mapping_drag_move(ggd.start_xform, delta);
      break;
    }
    case MappingHandle::Rotate: {
      const float2 start_dir = ggd.start_mouse_uv - ggd.drag_pivot_uv;
      const float2 dir = mouse_uv - ggd.drag_pivot_uv;
      if (math::length_squared(start_dir) < 1e-12f || math::length_squared(dir) < 1e-12f) {
        return;
      }
      /* Wrap the difference into (-pi, pi]: crossing the atan2 branch cut must not add a turn. */
      float raw_angle = std::atan2(dir.y, dir.x) - std::atan2(start_dir.y, start_dir.x);
      if (raw_angle > float(M_PI)) {
        raw_angle -= float(2.0 * M_PI);
      }
      else if (raw_angle <= -float(M_PI)) {
        raw_angle += float(2.0 * M_PI);
      }
      const float delta_angle = raw_angle * precision;
      next = uv_mapping_drag_rotate(ggd.start_xform, ggd.drag_pivot_uv, delta_angle);
      break;
    }
    default: {
      if (!handle_is_scale(ggd.handle)) {
        return;
      }
      const float2 mouse_eff = ggd.start_mouse_uv +
                               (mouse_uv - ggd.start_mouse_uv) * precision;
      next = uv_mapping_drag_scale(ggd.start_xform,
                                   ggd.drag_pivot_uv,
                                   ggd.start_mouse_uv,
                                   mouse_eff,
                                   scale_axes(ggd.handle),
                                   ggd.start_scale_locked);
      break;
    }
  }

  if (!target.write(next)) {
    finish_drag(C, ggd, false);
    return;
  }

  update_status_text(C, next);
  notify_mapping_changed(C, target.material(), CTX_wm_region(C));
}

wmOperatorStatus layer_mapping_gizmo_modal(bContext *C,
                                           wmGizmo *gz,
                                           const wmEvent *event,
                                           eWM_GizmoFlagTweak /*tweak_flag*/)
{
  LayerMappingGizmoGroup *ggd = layer_mapping_gizmo_group(gz->parent_gzgroup);
  ARegion *region = CTX_wm_region(C);
  if (ggd == nullptr || region == nullptr || event == nullptr) {
    return OPERATOR_FINISHED;
  }

  if (event->type == EVT_ESCKEY ||
      (event->type == RIGHTMOUSE && event->val == KM_PRESS))
  {
    finish_drag(C, *ggd, false);
    return OPERATOR_CANCELLED;
  }

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
    finish_drag(C, *ggd, true);
    return OPERATOR_FINISHED;
  }

  if (event->type != MOUSEMOVE || !ggd->drag_active) {
    return OPERATOR_RUNNING_MODAL;
  }

  const float2 mouse_uv = canvas_uv_from_event(*region, *event);

  if (ggd->handle == MappingHandle::Anchor) {
    anchor_place(*ggd, *region, *event);
    ED_region_tag_redraw(region);
    return OPERATOR_RUNNING_MODAL;
  }

  apply_drag(C, *ggd, event, mouse_uv);
  return OPERATOR_RUNNING_MODAL;
}

void layer_mapping_gizmo_invoke_prepare(const bContext *C,
                                        wmGizmoGroup *gzgroup,
                                        wmGizmo *gz,
                                        const wmEvent *event)
{
  LayerMappingGizmoGroup *ggd = layer_mapping_gizmo_group(gzgroup);
  if (ggd == nullptr || event == nullptr) {
    return;
  }

  MappingHandle handle = MappingHandle::None;
  if (gz == ggd->gz_anchor) {
    handle = MappingHandle::Anchor;
  }
  else if (gz == ggd->gz_cage) {
    handle = handle_from_cage_part(gz->highlight_part);
  }

  if (handle == MappingHandle::None) {
    return;
  }

  begin_drag(const_cast<bContext *>(C), *ggd, event, handle);
}

/** One of the two state buttons: the locked icon while the axes move together, else unlocked. */
wmGizmo *layer_mapping_lock_button_new(const wmGizmoType *gzt,
                                       wmGizmoGroup *gzgroup,
                                       const int icon)
{
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  /* The size is set every redraw in #layer_mapping_gizmo_draw_prepare: this group has no
   * #WM_GIZMOGROUPTYPE_SCALE, so the final scale is multiplied by the gizmo size preference.
   * Start from the same value so the first frame is not drawn at full gizmo size. */
  gz->scale_basis = LOCK_BUTTON_RADIUS / float(std::max<int>(U.gizmo_size, 1));
  RNA_enum_set(gz->ptr, "icon", icon);
  RNA_enum_set(gz->ptr, "draw_options",
               ED_GIZMO_BUTTON_SHOW_OUTLINE | ED_GIZMO_BUTTON_SHOW_BACKDROP);
  RNA_boolean_set(gz->ptr, "show_drag", false);
  wmOperatorType *ot = WM_operatortype_find("PAINT_OT_layer_mapping_scale_lock_toggle", false);
  if (ot != nullptr) {
    WM_gizmo_operator_set(gz, 0, ot, nullptr);
  }
  return gz;
}

/**
 * Decorative circle outline drawn over the visible lock button. `GIZMO_GT_button_2d` draws its own
 * outline in the same color as the fill (so it is invisible); this separate, non-selectable layer
 * adds a readable border in a contrasting theme color.
 */
wmGizmo *layer_mapping_lock_outline_new(const wmGizmoType *gzt, wmGizmoGroup *gzgroup)
{
  wmGizmo *gz = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  gz->flag |= WM_GIZMO_HIDDEN_SELECT;
  gz->line_width = LOCK_OUTLINE_WIDTH;
  RNA_enum_set(gz->ptr, "draw_options", ED_GIZMO_BUTTON_SHOW_BACKDROP);
  RNA_float_set(gz->ptr, "backdrop_fill_alpha", 0.0f);
  return gz;
}

/** Re-read the panel theme colors on every draw, so a theme change applies without a rebuild of
 * the gizmo map. */
static void layer_mapping_lock_colors_update(LayerMappingGizmoGroup *ggd)
{
  float color[4];
  ui::theme::get_color_4fv(TH_PANEL_BACK, color);
  WM_gizmo_set_color(ggd->gz_lock_locked, color);
  WM_gizmo_set_color(ggd->gz_lock_unlocked, color);
  ui::theme::get_color_4fv(TH_PANEL_HEADER, color);
  WM_gizmo_set_color_highlight(ggd->gz_lock_locked, color);
  WM_gizmo_set_color_highlight(ggd->gz_lock_unlocked, color);
  ui::theme::get_color_4fv(TH_PANEL_OUTLINE, color);
  WM_gizmo_set_color(ggd->gz_lock_outline, color);
  WM_gizmo_set_color_highlight(ggd->gz_lock_outline, color);
}

bool layer_mapping_gizmo_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }
  const SpaceImage *sima = CTX_wm_space_image(C);
  if (sima == nullptr || sima->mode != SI_MODE_PAINT) {
    return false;
  }
  PaintLayerRowMappingTarget target;
  return target.resolve(*C);
}

void layer_mapping_gizmo_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  const wmGizmoType *gzt_cage = WM_gizmotype_find("GIZMO_GT_cage_2d", true);
  const wmGizmoType *gzt_move3d = WM_gizmotype_find("GIZMO_GT_move_3d", true);

  LayerMappingGizmoGroup *ggd = MEM_new<LayerMappingGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *customdata) {
    MEM_delete(static_cast<LayerMappingGizmoGroup *>(customdata));
  };

  /* The lock buttons come first: the first gizmo hit wins, and the button sits inside the cage's
   * outer rotate zone. */
  const wmGizmoType *gzt_button = WM_gizmotype_find("GIZMO_GT_button_2d", true);
  ggd->gz_lock_locked = layer_mapping_lock_button_new(gzt_button, gzgroup, ICON_LOCKED);
  ggd->gz_lock_unlocked = layer_mapping_lock_button_new(gzt_button, gzgroup, ICON_UNLOCKED);
  /* Created after the buttons: gizmos draw in creation order, so a later one lies on top. Created
   * earlier, the button's own backdrop would hide the outline. */
  ggd->gz_lock_outline = layer_mapping_lock_outline_new(gzt_button, gzgroup);

  /* Anchor before cage so the pivot ring wins the hit test over the interior move region. */
  ggd->gz_anchor = WM_gizmo_new_ptr(gzt_move3d, gzgroup, nullptr);
  WM_gizmo_set_fn_custom_modal(ggd->gz_anchor, layer_mapping_gizmo_modal);
  RNA_enum_set(ggd->gz_anchor->ptr, "draw_style", ED_GIZMO_MOVE_STYLE_RING_2D);
  ggd->gz_anchor->flag |= WM_GIZMO_DRAW_MODAL | WM_GIZMO_DRAW_NO_SCALE;
  float anchor_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  float anchor_color_hi[4] = {1.0f, 1.0f, 1.0f, 1.0f};
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
                   ED_GIZMO_CAGE_DRAW_FLAG_CORNER_HANDLES |
                   ED_GIZMO_CAGE_DRAW_FLAG_ALL_HANDLES |
                   ED_GIZMO_CAGE_DRAW_FLAG_XFORM_INTERIOR_TRANSLATE |
                   ED_GIZMO_CAGE_DRAW_FLAG_DASHED_OUTLINE);
  WM_gizmo_set_fn_custom_modal(ggd->gz_cage, layer_mapping_gizmo_modal);
  ggd->gz_cage->flag |= WM_GIZMO_REFRESH_CURSOR_ON_MODAL;

  float cage_color[4] = {0.8f, 0.8f, 0.8f, 0.9f};
  float cage_color_hi[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  WM_gizmo_set_color(ggd->gz_cage, cage_color);
  WM_gizmo_set_color_highlight(ggd->gz_cage, cage_color_hi);
  WM_gizmo_set_line_width(ggd->gz_cage, 1.5f);
}

void layer_mapping_gizmo_refresh(const bContext *C, wmGizmoGroup *gzgroup)
{
  LayerMappingGizmoGroup *ggd = layer_mapping_gizmo_group(gzgroup);
  if (ggd == nullptr) {
    return;
  }

  PaintLayerRowMappingTarget target;
  const bool show = target.resolve(*C);

  if (show) {
    ggd->gz_cage->flag &= ~WM_GIZMO_HIDDEN;
    ggd->gz_anchor->flag &= ~WM_GIZMO_HIDDEN;

    const bool locked = target.scale_locked();
    RNA_enum_set(ggd->gz_cage->ptr,
                 "transform",
                 ED_GIZMO_CAGE_XFORM_FLAG_TRANSLATE | ED_GIZMO_CAGE_XFORM_FLAG_ROTATE |
                     (locked ? ED_GIZMO_CAGE_XFORM_FLAG_SCALE_UNIFORM :
                               ED_GIZMO_CAGE_XFORM_FLAG_SCALE));

    if (locked) {
      ggd->gz_lock_locked->flag &= ~WM_GIZMO_HIDDEN;
      ggd->gz_lock_unlocked->flag |= WM_GIZMO_HIDDEN;
    }
    else {
      ggd->gz_lock_locked->flag |= WM_GIZMO_HIDDEN;
      ggd->gz_lock_unlocked->flag &= ~WM_GIZMO_HIDDEN;
    }
    ggd->gz_lock_outline->flag &= ~WM_GIZMO_HIDDEN;
  }
  else {
    ggd->gz_cage->flag |= WM_GIZMO_HIDDEN;
    ggd->gz_anchor->flag |= WM_GIZMO_HIDDEN;
    ggd->gz_lock_locked->flag |= WM_GIZMO_HIDDEN;
    ggd->gz_lock_unlocked->flag |= WM_GIZMO_HIDDEN;
    ggd->gz_lock_outline->flag |= WM_GIZMO_HIDDEN;
  }

  /* A native gizmo tweak ends on release before custom_modal sees the release: settle the
   * gesture here when our modal is gone. */
  const ARegion *region = CTX_wm_region(C);
  wmGizmoMap *gzmap = (region != nullptr && region->runtime != nullptr) ?
                          region->runtime->gizmo_map :
                          nullptr;
  wmGizmo *modal_gz = gzmap != nullptr ? WM_gizmomap_get_modal(gzmap) : nullptr;
  const bool is_our_modal = modal_gz != nullptr &&
                            (modal_gz == ggd->gz_cage || modal_gz == ggd->gz_anchor);
  /* No "was modal on an earlier redraw" requirement, as in the shape gizmo: the handle is armed
   * and the modal started within one event, while a quick click or a slow redraw can press and
   * release between two refreshes. A refresh that finds a drag without our modal always means the
   * tweak is over. A drag that made no change pushes no undo step. */
  if (!is_our_modal && ggd->drag_active) {
    /* Esc and a right click reach the tweak operator as its modal map and never our modal, so the
     * last event is the only trace of a cancel. */
    const wmWindow *win = CTX_wm_window(C);
    const wmEvent *last_event = win != nullptr ? win->runtime->eventstate : nullptr;
    const bool cancelled = last_event != nullptr && last_event->val == KM_PRESS &&
                           ELEM(last_event->type, EVT_ESCKEY, RIGHTMOUSE);
    finish_drag(const_cast<bContext *>(C), *ggd, !cancelled);
  }
}

void layer_mapping_gizmo_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  LayerMappingGizmoGroup *ggd = layer_mapping_gizmo_group(gzgroup);
  ARegion *region = CTX_wm_region(C);
  if (ggd == nullptr || region == nullptr) {
    return;
  }

  PaintLayerRowMappingTarget target;
  if (!target.resolve(*C)) {
    return;
  }

  UvMappingXform xform;
  if (!target.read(xform)) {
    return;
  }

  const CageScreenGeometry geo = cage_screen_geometry(*region, xform);
  if (!geo.valid) {
    return;
  }
  const float2 center = geo.center;
  const float2 corner11 = geo.corners[2];
  const float2 edge_x = geo.corners[1] - geo.corners[0];
  const float2 edge_y = geo.corners[3] - geo.corners[0];

  unit_m4(ggd->gz_cage->matrix_space);
  unit_m4(ggd->gz_cage->matrix_basis);
  unit_m4(ggd->gz_cage->matrix_offset);
  ggd->gz_cage->matrix_offset[0][0] = edge_x.x;
  ggd->gz_cage->matrix_offset[0][1] = edge_x.y;
  ggd->gz_cage->matrix_offset[1][0] = edge_y.x;
  ggd->gz_cage->matrix_offset[1][1] = edge_y.y;
  ggd->gz_cage->matrix_offset[3][0] = center.x;
  ggd->gz_cage->matrix_offset[3][1] = center.y;

  float2 anchor_uv;
  if (ggd->drag_active && ggd->handle == MappingHandle::Anchor) {
    anchor_uv = ggd->drag_pivot_uv;
  }
  else if (ggd->custom_pivot && BLI_uuid_equal(ggd->custom_pivot_marker, target.marker())) {
    anchor_uv = uv_mapping_footprint_point(xform, ggd->custom_pivot_map);
  }
  else {
    anchor_uv = uv_mapping_footprint_center(xform);
  }
  const float2 anchor = region_from_canvas_uv(*region, anchor_uv);

  unit_m4(ggd->gz_anchor->matrix_space);
  unit_m4(ggd->gz_anchor->matrix_basis);
  unit_m4(ggd->gz_anchor->matrix_offset);
  const float anchor_location[3] = {anchor.x, anchor.y, 0.0f};
  WM_gizmo_set_matrix_location(ggd->gz_anchor, anchor_location);

  /* Scale-lock toggle just outside the cage's top-right corner. */
  const float2 lock_px = corner11 + float2(LOCK_BUTTON_OFFSET, LOCK_BUTTON_OFFSET);
  const float lock_location[3] = {lock_px.x, lock_px.y, 0.0f};
  wmGizmo *lock_gizmos[3] = {ggd->gz_lock_locked, ggd->gz_lock_unlocked, ggd->gz_lock_outline};
  /* `scale_final = scale_basis * UI_SCALE_FAC * U.gizmo_size` for a group without the SCALE flag
   * (see `wm_gizmo_calculate_scale`), and the button's circle is drawn with that as its radius in
   * pixels. Dividing the gizmo size out leaves a radius of #LOCK_BUTTON_RADIUS UI pixels. */
  const float lock_scale_basis = LOCK_BUTTON_RADIUS / float(std::max<int>(U.gizmo_size, 1));
  for (wmGizmo *gz : lock_gizmos) {
    gz->scale_basis = lock_scale_basis;
    unit_m4(gz->matrix_space);
    unit_m4(gz->matrix_basis);
    unit_m4(gz->matrix_offset);
    WM_gizmo_set_matrix_location(gz, lock_location);
  }
  layer_mapping_lock_colors_update(ggd);
}

}  // namespace

void ED_image_paint_layer_mapping_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Paint Layer Mapping";
  gzgt->idname = "IMAGE_GGT_paint_layer_mapping";

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->poll = layer_mapping_gizmo_poll;
  gzgt->setup = layer_mapping_gizmo_setup;
  gzgt->invoke_prepare = layer_mapping_gizmo_invoke_prepare;
  /* Press, not click-drag: the lock buttons are single-click actions, and the "maybe drag" keymap
   * only starts a tweak on a drag, so a plain click never reached them. A Fill row is not a stroke
   * target, so a press on the cage cannot clash with painting. */
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic;
  gzgt->refresh = layer_mapping_gizmo_refresh;
  gzgt->draw_prepare = layer_mapping_gizmo_draw_prepare;
}

}  // namespace blender
