/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The 3D Viewport handles of the active transform effect (Transform Mapping, Mirror): a point where
 * the effect's UV pivot (or the Mirror's fold center) lies on the mesh surface, and for a Transform
 * Mapping a rotation handle beside it. Dragging the point places the pivot under the cursor without
 * moving what the effect shows; dragging the handle turns the effect about the pivot.
 *
 * It edits the same data as the Image Editor's mapping gizmo, through the same
 * #PaintLayerRowMappingTarget, so both views and the Mapping panel stay on one set of values and one
 * set of undo steps. The point is placed through the UV-to-surface and surface-to-UV helpers the
 * Gradient Layer handles use.
 */

#include <cmath>

#include "MEM_guardedalloc.h"

#include "BLI_index_range.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_userdef_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_paint_layers.hh"

#include "ED_gizmo_library.hh"
#include "ED_material_combined.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "GPU_immediate.hh"
#include "GPU_immediate_util.hh"
#include "GPU_shader_builtin.hh"
#include "GPU_state.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../paint_curve_screen.hh"
#include "../paint_gradient_gizmo.hh"
#include "../paint_layers_gradient.hh"
#include "uv_mapping_math.hh"
#include "uv_mapping_target.hh"

namespace blender {

namespace {

using namespace ed::sculpt_paint;
using namespace ed::sculpt_paint::uv_mapping;

/** What a drag moves. The numbers are the gizmo's part indices (0 is "nothing"). */
enum class UvHandle : int {
  None = 0,
  Pivot = 1,
  Rotate = 2,
  /** The footprint center: moves the tile, which is the mapping's offset. */
  Move = 3,
  /** The scale handle: scales both axes together. Only offered while the scale is locked, the axes
   * of an unlocked scale are edited in the Mapping panel. */
  Scale = 4,
};

/** Number of footprint handles placed on the surface: only the offset point. */
constexpr int TILE_HANDLES_NUM = 1;

/**
 * The rotation and scale handles are screen-space, the way the Curve Patch point handles are: the
 * scale handle is a knob on a fixed screen axis (the radius handle's, see
 * #paintcurve_radius_from_handle_screen_pos) and the rotation handle a knob on an arc whose angle
 * follows the cursor about the pivot. Neither is tied to the surface, so they can be pulled as far
 * out as the view allows, whatever the UVs do there.
 */
/** Screen radius of the rotation arc around the pivot, in pixels. */
constexpr float ROTATE_ARC_RADIUS = 64.0f;
/** Screen distance of the offset point from the pivot, in UI pixels. */
constexpr float OFFSET_HANDLE_DISTANCE = 44.0f;
/** The scale factor the handle is drawn at is clamped here: it is `1 / scale`, so this bounds the
 * scale a drag can reach. */
constexpr float SCALE_FACTOR_MIN = 0.02f;
/** UV step the screen axes are measured over: small enough to stay on one triangle. */
constexpr float AXIS_STEP_UV = 0.01f;

struct UvTransformGizmoGroup {
  wmGizmo *gizmo = nullptr;

  bool points_valid = false;
  /** A Mirror effect has a center and nothing to turn. */
  bool is_mirror = false;
  bool rotate_valid = false;
  bool scale_valid = false;
  float2 pivot_px = float2(0.0f);
  /** The rotation knob on its arc, and the screen angle the arc is centered on. */
  float2 handle_px = float2(0.0f);
  float rotate_angle = 0.0f;
  /** +1 when the surface keeps the canvas's handedness on screen, -1 when it mirrors it: the sign
   * that maps a screen-space turn onto the canvas, as the Curve Patch angle handle's does. */
  float rotate_sign = 1.0f;
  /** Screen vectors of one UV unit along U and V at the pivot: the surface's own axes. */
  float2 axis_u_px = float2(0.0f);
  float2 axis_v_px = float2(0.0f);
  bool axes_valid = false;
  /** The scale knob, the screen axis it sits on, and the factor `1 / scale` it is drawn at. */
  float2 scale_px = float2(0.0f);
  float2 scale_dir = float2(0.0f, 1.0f);
  float scale_factor = 1.0f;

  /** The footprint handles on the surface, indexed by #UvHandle minus #UvHandle::Move. */
  bool tile_valid[TILE_HANDLES_NUM] = {};
  float2 tile_px[TILE_HANDLES_NUM] = {};
  int tri_tile[TILE_HANDLES_NUM] = {-1};

  /** Surface triangle of the pivot and of its two probes, to start the next search at. */
  int tri_pivot = -1;
  int tri_u = -1;
  int tri_v = -1;
  /** What the placement above was computed for, to drop the caches when it changes. */
  const Mesh *cache_mesh = nullptr;
  const Object *cache_object = nullptr;

  /* The running drag. */
  UvHandle drag_handle = UvHandle::None;
  UvMappingXform start_xform;
  float2 start_point = float2(0.0f);
  /** The screen angle of the press about the pivot, for the rotation drag. */
  float start_angle = 0.0f;
  /** The scale handle's axis, fixed in region pixels at the press, and the factor it started at. */
  PaintCurveRadiusHandleScreen scale_axis = {};
  float start_scale_factor = 1.0f;
  /** The press in region pixels and the surface axes frozen at it, for the offset drag. */
  float2 start_press_px = float2(0.0f);
  float2 drag_axis_u = float2(0.0f);
  float2 drag_axis_v = float2(0.0f);
  Material *session_material = nullptr;
  uint32_t session_material_uid = 0;
  bUUID session_marker = {};
};

UvTransformGizmoGroup *group_data(wmGizmoGroup *gzgroup)
{
  return static_cast<UvTransformGizmoGroup *>(gzgroup->customdata);
}

UvTransformGizmoGroup *group_data(const wmGizmo *gz)
{
  return group_data(gz->parent_gzgroup);
}

/**
 * The transform effect the gizmo edits: the shown row when it is a Transform Mapping or a Mirror.
 * False for any other row, so the handles are only there while such an effect is active.
 */
bool target_resolve(const bContext &C, PaintLayerRowMappingTarget &r_target)
{
  if (!r_target.resolve_row(C, bUUID{})) {
    return false;
  }
  return r_target.has_pivot() || r_target.is_mirror_effect();
}

/** The UV point the handles sit at: the pivot, or the Mirror's center. */
bool point_read(const PaintLayerRowMappingTarget &target, float2 &r_point)
{
  if (target.has_pivot()) {
    return target.read_pivot(r_point);
  }
  float center[2];
  if (!target.read_mirror_center(center)) {
    return false;
  }
  r_point = float2(center[0], center[1]);
  return true;
}

bool point_write(PaintLayerRowMappingTarget &target, const float2 &point)
{
  return target.has_pivot() ? target.write_pivot(point) : target.write_mirror_center(point);
}

void notify_changed(bContext &C, Material &material)
{
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
  if (ARegion *region = CTX_wm_region(&C)) {
    ED_region_tag_redraw(region);
  }
}

/** Project the UV point \a uv to region pixels through the surface; false when it is not on screen. */
bool uv_to_px(const ARegion &region,
              const Object &ob,
              const Mesh &mesh,
              const float2 &uv,
              int &r_tri,
              float2 &r_px)
{
  float3 world;
  if (!paint_layer_uv_world_point(ob, mesh, uv, r_tri, world)) {
    return false;
  }
  float projected[2];
  if (ED_view3d_project_float_global(&region, world, projected, V3D_PROJ_TEST_CLIP_DEFAULT) !=
      V3D_PROJ_RET_OK)
  {
    return false;
  }
  r_px = float2(projected[0], projected[1]);
  return true;
}

/** Fill the screen points from the effect's values; false when there is nothing to draw. */
bool points_update(const bContext &C, UvTransformGizmoGroup &ggd)
{
  ARegion *region = CTX_wm_region(&C);
  Object *ob = CTX_data_active_object(&C);
  PaintLayerRowMappingTarget target;
  if (region == nullptr || ob == nullptr || !target_resolve(C, target)) {
    return false;
  }
  const Mesh *mesh = paint_layer_gradient_mesh_eval(C, *ob);
  if (mesh == nullptr) {
    return false;
  }
  if (ggd.cache_mesh != mesh || ggd.cache_object != ob) {
    ggd.cache_mesh = mesh;
    ggd.cache_object = ob;
    ggd.tri_pivot = ggd.tri_u = ggd.tri_v = -1;
    for (int &tri : ggd.tri_tile) {
      tri = -1;
    }
  }

  float2 point;
  if (!point_read(target, point) || !uv_to_px(*region, *ob, *mesh, point, ggd.tri_pivot, ggd.pivot_px))
  {
    return false;
  }
  ggd.is_mirror = target.is_mirror_effect();
  ggd.handle_px = ggd.pivot_px;
  ggd.rotate_valid = false;
  ggd.scale_valid = false;
  ggd.axes_valid = false;
  for (bool &valid : ggd.tile_valid) {
    valid = false;
  }
  if (ggd.is_mirror) {
    return true;
  }
  UvMappingXform xform;
  if (!target.read(xform)) {
    return true;
  }

  /* The surface's U and V directions at the pivot, as seen on screen. They give the rotation knob
   * the screen angle the stamp's texture would have, and the sign that maps a screen turn onto the
   * canvas (a mirrored island turns the other way round). */
  float2 u_px;
  float2 v_px;
  float2 screen_dir(1.0f, 0.0f);
  if (uv_to_px(*region, *ob, *mesh, point + float2(AXIS_STEP_UV, 0.0f), ggd.tri_u, u_px) &&
      uv_to_px(*region, *ob, *mesh, point + float2(0.0f, AXIS_STEP_UV), ggd.tri_v, v_px))
  {
    const float2 axis_u = (u_px - ggd.pivot_px) / AXIS_STEP_UV;
    const float2 axis_v = (v_px - ggd.pivot_px) / AXIS_STEP_UV;
    const float det = axis_u.x * axis_v.y - axis_u.y * axis_v.x;
    ggd.axis_u_px = axis_u;
    ggd.axis_v_px = axis_v;
    ggd.axes_valid = std::fabs(det) >= 1e-3f;
    /* The footprint turns by minus the stored rotation in the canvas (see the Image Editor cage),
     * so the knob points along that canvas direction. */
    const float2 canvas_dir(std::cos(xform.rotation), -std::sin(xform.rotation));
    const float2 mapped = axis_u * canvas_dir.x + axis_v * canvas_dir.y;
    if (std::fabs(det) >= 1e-3f && math::length_squared(mapped) > 1e-12f) {
      screen_dir = math::normalize(mapped);
      ggd.rotate_sign = det > 0.0f ? 1.0f : -1.0f;
      ggd.rotate_valid = true;
    }
  }
  ggd.rotate_angle = std::atan2(screen_dir.y, screen_dir.x);
  ggd.handle_px = ggd.pivot_px + screen_dir * (ROTATE_ARC_RADIUS * UI_SCALE_FAC);

  /* The offset point hangs on the side opposite the scale handle, at a fixed screen distance from
   * the pivot. It is not tied to where the footprint's center lands on the surface: an offset
   * past the UV range puts that center outside every island, where no surface point exists to
   * grab. The drag turns the cursor's screen travel into UV travel through the surface axes. */
  ggd.tile_valid[0] = ggd.axes_valid;
  ggd.tile_px[0] = ggd.pivot_px +
                   float2(screen_dir.y, -screen_dir.x) * (OFFSET_HANDLE_DISTANCE * UI_SCALE_FAC);

  /* The scale handle hangs a quarter turn from the rotation knob, at `1 / scale` along its axis, so
   * pulling it out grows the content. It exists only while the scale is locked: the axes of an
   * unlocked scale are edited in the Mapping panel. */
  if (target.scale_locked()) {
    ggd.scale_dir = float2(-screen_dir.y, screen_dir.x);
    const float scale = std::max(std::fabs(xform.scale.x), 1e-3f);
    ggd.scale_factor = std::max(1.0f / scale, SCALE_FACTOR_MIN);
    ggd.scale_px = ggd.pivot_px +
                   ggd.scale_dir * (PAINT_CURVE_RADIUS_HANDLE_MIN_OFFSET +
                                    ggd.scale_factor * PAINT_CURVE_RADIUS_HANDLE_BASE_LEN);
    ggd.scale_valid = true;
  }
  return true;
}

/** \a angle wrapped into (-pi, pi]: crossing the atan2 branch cut must not add a turn. */
float angle_wrap(const float angle)
{
  if (angle > float(M_PI)) {
    return angle - float(2.0 * M_PI);
  }
  if (angle <= -float(M_PI)) {
    return angle + float(2.0 * M_PI);
  }
  return angle;
}

/* -------------------------------------------------------------------- */
/** \name Dragging
 * \{ */

void drag_finish(bContext &C, wmGizmo *gz, const bool commit)
{
  UvTransformGizmoGroup &ggd = *group_data(gz);
  if (ggd.drag_handle == UvHandle::None) {
    return;
  }
  /* By uid, like the drag's start: the material may be gone by the time the drag ends. */
  ed::material_combined::gradient_interactive_end(ggd.session_material_uid);

  PaintLayerRowMappingTarget target;
  const bool same_target = target_resolve(C, target) && target.material() == ggd.session_material &&
                           BLI_uuid_equal(target.marker(), ggd.session_marker);
  Material *material = same_target ? target.material() : ggd.session_material;
  if (same_target) {
    if (ggd.drag_handle == UvHandle::Pivot) {
      float2 current;
      if (commit) {
        if (point_read(target, current) &&
            math::distance_squared(current, ggd.start_point) > 1e-12f)
        {
          ED_undo_push(&C, target.has_pivot() ? target.pivot_undo_name() : target.mirror_undo_name());
        }
      }
      else {
        point_write(target, ggd.start_point);
      }
    }
    else if (commit) {
      UvMappingXform current;
      if (target.read(current) &&
          (math::distance_squared(current.offset, ggd.start_xform.offset) > 1e-12f ||
           math::distance_squared(current.scale, ggd.start_xform.scale) > 1e-12f ||
           std::fabs(current.rotation - ggd.start_xform.rotation) > 1e-6f))
      {
        ED_undo_push(&C, target.undo_name());
      }
    }
    else {
      target.write(ggd.start_xform);
    }
  }
  if (material != nullptr && same_target) {
    notify_changed(C, *material);
  }
  else if (ARegion *region = CTX_wm_region(&C)) {
    ED_region_tag_redraw(region);
  }
  ggd.drag_handle = UvHandle::None;
  ggd.session_material_uid = 0;
}

void drag_begin(bContext &C, wmGizmo *gz, const wmEvent &event)
{
  UvTransformGizmoGroup &ggd = *group_data(gz);
  PaintLayerRowMappingTarget target;
  if (!target_resolve(C, target) || gz->highlight_part == int(UvHandle::None)) {
    return;
  }
  const UvHandle handle = UvHandle(gz->highlight_part);
  /* A Mirror has one handle; whichever disc was picked moves its center. */
  ggd.drag_handle = ggd.is_mirror ? UvHandle::Pivot : handle;
  ggd.session_material = target.material();
  ggd.session_material_uid = target.material()->id.session_uid;
  ggd.session_marker = target.marker();
  target.read(ggd.start_xform);
  point_read(target, ggd.start_point);
  ggd.start_angle = 0.0f;
  const float2 press_px = float2(float(event.mval[0]), float(event.mval[1]));
  if (ggd.drag_handle == UvHandle::Rotate) {
    /* Screen angle of the press about the pivot: every move rides on its difference, like the
     * Curve Patch rotation handle. */
    const float2 from_pivot = press_px - ggd.pivot_px;
    if (math::length_squared(from_pivot) < 1.0f) {
      ggd.drag_handle = UvHandle::None;
      return;
    }
    ggd.start_angle = std::atan2(from_pivot.y, from_pivot.x);
  }
  else if (ggd.drag_handle == UvHandle::Scale) {
    /* The axis is fixed in region pixels at the press, so every move just re-projects the cursor
     * onto it -- the inversion the Curve Patch radius handle uses. */
    ggd.scale_axis.point = ggd.pivot_px;
    ggd.scale_axis.perp = ggd.scale_dir;
    ggd.scale_axis.end = ggd.scale_px;
    ggd.start_scale_factor = ggd.scale_factor;
  }
  else if (ggd.drag_handle == UvHandle::Move) {
    /* The offset point is screen-space like the others: the cursor's travel from the press is turned
     * into UV travel through the surface axes at the pivot, fixed here so the drag cannot change
     * its own scale. It needs no surface under the cursor, nor under the footprint's center. */
    if (!ggd.axes_valid) {
      ggd.drag_handle = UvHandle::None;
      return;
    }
    ggd.start_press_px = press_px;
    ggd.drag_axis_u = ggd.axis_u_px;
    ggd.drag_axis_v = ggd.axis_v_px;
  }
  /* The composite previews shade at an octave while the drag lasts and refine when it ends: a full
   * rebuild on every mouse move is what makes a large canvas lag. */
  ed::material_combined::gradient_interactive_begin(*target.material());
}

void drag_apply(bContext &C, wmGizmo *gz, const wmEvent &event)
{
  UvTransformGizmoGroup &ggd = *group_data(gz);
  PaintLayerRowMappingTarget target;
  if (!target_resolve(C, target) || target.material() != ggd.session_material ||
      !BLI_uuid_equal(target.marker(), ggd.session_marker))
  {
    /* The row or its material changed mid-drag: stop without another write. */
    drag_finish(C, gz, false);
    return;
  }
  /* Re-armed on every step: the mark expires on its own, and a drag held still must not lose it. */
  ed::material_combined::gradient_interactive_begin(*target.material());
  const float2 mval(float(event.mval[0]), float(event.mval[1]));
  const float precision = (event.modifier & KM_SHIFT) ? 0.1f : 1.0f;

  if (ggd.drag_handle == UvHandle::Pivot) {
    PaintLayerGradientHit hit;
    if (!paint_layer_uv_hit_at_mouse(C, mval, hit) || !hit.valid || !hit.has_uv) {
      /* Off the surface the point stays where it was, rather than jumping to a made-up UV. */
      return;
    }
    if (point_write(target, hit.uv)) {
      notify_changed(C, *target.material());
    }
    return;
  }

  if (ggd.drag_handle == UvHandle::Move) {
    /* Solve `delta_px = u * du + v * dv` for the UV travel, the footprint following the cursor. */
    const float2 delta_px = mval - ggd.start_press_px;
    const float det = ggd.drag_axis_u.x * ggd.drag_axis_v.y - ggd.drag_axis_u.y * ggd.drag_axis_v.x;
    if (std::fabs(det) < 1e-3f) {
      return;
    }
    const float2 delta_uv((delta_px.x * ggd.drag_axis_v.y - delta_px.y * ggd.drag_axis_v.x) / det,
                          (ggd.drag_axis_u.x * delta_px.y - ggd.drag_axis_u.y * delta_px.x) / det);
    const UvMappingXform next = uv_mapping_drag_move(ggd.start_xform, delta_uv * precision);
    if (target.write(next)) {
      notify_changed(C, *target.material());
    }
    return;
  }

  if (ggd.drag_handle == UvHandle::Scale) {
    /* The cursor's distance along the fixed axis is the factor `1 / scale`. Shift narrows the change
     * to a tenth of the way from the press value. */
    const float pos[2] = {mval.x, mval.y};
    float factor = paintcurve_radius_from_handle_screen_pos(&ggd.scale_axis, pos);
    factor = ggd.start_scale_factor + (factor - ggd.start_scale_factor) * precision;
    factor = std::max(factor, SCALE_FACTOR_MIN);
    const float magnitude = 1.0f / factor;
    /* The sign of each axis stays: a negative scale is a mirror the user made on purpose. */
    const float2 scale(std::copysign(magnitude, ggd.start_xform.scale.x),
                       std::copysign(magnitude, ggd.start_xform.scale.y));
    if (target.write_pivoted_scale(scale)) {
      notify_changed(C, *target.material());
    }
    return;
  }

  /* Rotation: the screen angle's delta since the press rides on the press-time rotation, and the
   * sign maps it onto the canvas when the surface mirrors it. The footprint turns by the canvas
   * delta while the stored rotation decreases by it. Ctrl snaps to 15 degrees. */
  const float2 from_pivot = mval - ggd.pivot_px;
  if (math::length_squared(from_pivot) < 1.0f) {
    return;
  }
  float delta = angle_wrap(std::atan2(from_pivot.y, from_pivot.x) - ggd.start_angle);
  if ((event.modifier & KM_CTRL) != 0) {
    const float snap = float(M_PI) / 12.0f;
    delta = std::round(delta / snap) * snap;
  }
  const float canvas_delta = ggd.rotate_sign * delta * precision;
  if (target.write_pivoted_rotation(ggd.start_xform.rotation - canvas_delta)) {
    notify_changed(C, *target.material());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Gizmo callbacks
 * \{ */

void gizmo_draw(const bContext * /*C*/, wmGizmo *gz)
{
  const UvTransformGizmoGroup &ggd = *group_data(gz);
  if (!ggd.points_valid) {
    return;
  }
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_line_smooth(true);
  const GPUDepthTest depth_prev = GPU_depth_test_get();
  GPU_depth_test(GPU_DEPTH_NONE);

  GPUVertFormat *format = immVertexFormat();
  const uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
  constexpr int segments = 16;
  const float radius = gradient_gizmo::HANDLE_RADIUS;
  const auto disc = [&](const float2 &center, const float r, const float4 &fill) {
    immUniformColor4f(fill.x, fill.y, fill.z, fill.w);
    imm_draw_circle_fill_3d(pos, center.x, center.y, r, segments);
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.8f);
    GPU_line_width(1.0f);
    imm_draw_circle_wire_3d(pos, center.x, center.y, r, segments);
  };
  /* Every handle hangs off the pivot on a line, so they read as one gizmo: the offset point, the
   * scale handle (only while the scale is locked) and the rotation handle. */
  GPU_line_width(1.5f);
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.7f);
  const auto link = [&](const float2 &to) {
    immBegin(GPU_PRIM_LINES, 2);
    immVertex3f(pos, ggd.pivot_px.x, ggd.pivot_px.y, 0.0f);
    immVertex3f(pos, to.x, to.y, 0.0f);
    immEnd();
  };
  for (const int i : IndexRange(TILE_HANDLES_NUM)) {
    if (ggd.tile_valid[i]) {
      link(ggd.tile_px[i]);
    }
  }
  if (ggd.scale_valid) {
    link(ggd.scale_px);
  }
  if (ggd.rotate_valid) {
    /* The knob sits on a half-circle arc centered on its angle, the Curve Patch rotation handle's
     * shape, and hangs off the pivot on a line like the others. */
    link(ggd.handle_px);
    const float arc_radius = ROTATE_ARC_RADIUS * UI_SCALE_FAC;
    constexpr int arc_segments = 24;
    immBegin(GPU_PRIM_LINE_STRIP, arc_segments + 1);
    for (const int i : IndexRange(arc_segments + 1)) {
      const float angle = ggd.rotate_angle - float(M_PI_2) +
                          float(M_PI) * float(i) / float(arc_segments);
      immVertex3f(pos,
                  ggd.pivot_px.x + std::cos(angle) * arc_radius,
                  ggd.pivot_px.y + std::sin(angle) * arc_radius,
                  0.0f);
    }
    immEnd();
  }

  if (ggd.tile_valid[0]) {
    /* The offset point is a small move cross -- a point with four arrowheads -- so it reads as
     * something to drag that places the tile. */
    const float2 c = ggd.tile_px[0];
    const float arm = radius * 0.8f;
    const float head = radius * 0.35f;
    immUniformColor4f(0.0f, 0.0f, 0.0f, 0.85f);
    GPU_line_width(3.5f);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex3f(pos, c.x - arm, c.y, 0.0f);
    immVertex3f(pos, c.x + arm, c.y, 0.0f);
    immVertex3f(pos, c.x, c.y - arm, 0.0f);
    immVertex3f(pos, c.x, c.y + arm, 0.0f);
    immEnd();
    immUniformColor4f(0.35f, 0.75f, 1.0f, 1.0f);
    GPU_line_width(1.5f);
    immBegin(GPU_PRIM_LINES, 4);
    immVertex3f(pos, c.x - arm, c.y, 0.0f);
    immVertex3f(pos, c.x + arm, c.y, 0.0f);
    immVertex3f(pos, c.x, c.y - arm, 0.0f);
    immVertex3f(pos, c.x, c.y + arm, 0.0f);
    immEnd();
    immBegin(GPU_PRIM_TRIS, 12);
    const float2 directions[4] = {
        float2(1.0f, 0.0f), float2(-1.0f, 0.0f), float2(0.0f, 1.0f), float2(0.0f, -1.0f)};
    for (const float2 &dir : directions) {
      const float2 tip = c + dir * (arm + head);
      const float2 base = c + dir * arm;
      const float2 side = float2(-dir.y, dir.x) * head;
      immVertex3f(pos, tip.x, tip.y, 0.0f);
      immVertex3f(pos, base.x + side.x, base.y + side.y, 0.0f);
      immVertex3f(pos, base.x - side.x, base.y - side.y, 0.0f);
    }
    immEnd();
  }
  if (ggd.scale_valid) {
    disc(ggd.scale_px, radius * 0.6f, float4(1.0f, 0.75f, 0.25f, 0.95f));
  }
  if (ggd.rotate_valid) {
    disc(ggd.handle_px, radius * 0.6f, float4(0.6f, 1.0f, 0.5f, 0.95f));
  }
  disc(ggd.pivot_px, radius * 0.9f, float4(0.9f, 0.9f, 0.9f, 0.95f));
  immUnbindProgram();

  GPU_depth_test(depth_prev);
  GPU_line_smooth(false);
  GPU_blend(GPU_BLEND_NONE);
}

int gizmo_test_select(bContext * /*C*/, wmGizmo *gz, const int mval[2])
{
  const UvTransformGizmoGroup &ggd = *group_data(gz);
  if (!ggd.points_valid) {
    return -1;
  }
  const float2 mouse = float2(float(mval[0]), float(mval[1]));
  constexpr float pick_radius = gradient_gizmo::HANDLE_RADIUS * 1.8f;
  if (math::distance(mouse, ggd.pivot_px) <= pick_radius) {
    return int(UvHandle::Pivot);
  }
  if (ggd.rotate_valid && math::distance(mouse, ggd.handle_px) <= pick_radius) {
    return int(UvHandle::Rotate);
  }
  if (ggd.scale_valid && math::distance(mouse, ggd.scale_px) <= pick_radius) {
    return int(UvHandle::Scale);
  }
  if (ggd.tile_valid[0] && math::distance(mouse, ggd.tile_px[0]) <= pick_radius) {
    return int(UvHandle::Move);
  }
  return -1;
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
  UvTransformGizmoGroup &ggd = *group_data(gz);
  if (C == nullptr || event == nullptr || ggd.drag_handle == UvHandle::None) {
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
  UvTransformGizmoGroup &ggd = *group_data(gzgroup);
  if (ggd.drag_handle == UvHandle::None) {
    return;
  }
  PaintLayerRowMappingTarget target;
  const bool same = target_resolve(*C, target) && target.material() == ggd.session_material &&
                    BLI_uuid_equal(target.marker(), ggd.session_marker);
  if (!same) {
    /* The active row, its material or the object changed: the drag cannot write anymore. */
    drag_finish(*const_cast<bContext *>(C), ggd.gizmo, false);
  }
}

void group_draw_prepare(const bContext *C, wmGizmoGroup *gzgroup)
{
  UvTransformGizmoGroup &ggd = *group_data(gzgroup);
  ggd.points_valid = points_update(*C, ggd);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Group setup
 * \{ */

bool group_poll(const bContext *C, wmGizmoGroupType * /*gzgt*/)
{
  if ((U.gizmo_flag & USER_GIZMO_DRAW) == 0) {
    return false;
  }
  const View3D *v3d = CTX_wm_view3d(C);
  if (v3d == nullptr || (v3d->gizmo_flag & V3D_GIZMO_HIDE) != 0) {
    return false;
  }
  const Object *ob = CTX_data_active_object(C);
  if (ob == nullptr || ob->type != OB_MESH ||
      (ob->mode & (OB_MODE_SCULPT | OB_MODE_TEXTURE_PAINT)) == 0)
  {
    return false;
  }
  PaintLayerRowMappingTarget target;
  return target_resolve(*C, target);
}

void group_setup(const bContext * /*C*/, wmGizmoGroup *gzgroup)
{
  auto *ggd = MEM_new<UvTransformGizmoGroup>(__func__);
  gzgroup->customdata = ggd;
  gzgroup->customdata_free = [](void *customdata) {
    UvTransformGizmoGroup *group = static_cast<UvTransformGizmoGroup *>(customdata);
    /* A drag this group never got to finish leaves a mark nothing else clears. */
    ed::material_combined::gradient_interactive_end(group->session_material_uid);
    MEM_delete(group);
  };

  const wmGizmoType *gzt = WM_gizmotype_find("PAINT_GT_layer_uv_transform", true);
  if (gzt == nullptr) {
    return;
  }
  ggd->gizmo = WM_gizmo_new_ptr(gzt, gzgroup, nullptr);
  WM_gizmo_set_fn_custom_modal(ggd->gizmo, gizmo_modal);
  /* A modal gizmo is otherwise skipped while it is dragged. */
  WM_gizmo_set_flag(ggd->gizmo, WM_GIZMO_DRAW_MODAL, true);
}

void gizmo_type_setup(wmGizmoType *gzt)
{
  gzt->idname = "PAINT_GT_layer_uv_transform";
  gzt->struct_size = sizeof(wmGizmo);

  gzt->draw = gizmo_draw;
  gzt->test_select = gizmo_test_select;
  gzt->cursor_get = gizmo_cursor_get;
}

}  // namespace

/** \} */

void ED_gizmotypes_paint_layer_uv_transform()
{
  static bool registered = false;
  if (registered) {
    return;
  }
  registered = true;
  WM_gizmotype_append(gizmo_type_setup);
}

void ED_view3d_paint_layer_uv_transform_gizmo_setup(wmGizmoGroupType *gzgt)
{
  gzgt->name = "Transform Effect Handles";
  gzgt->idname = "VIEW3D_GGT_paint_layer_uv_transform";

  gzgt->flag = WM_GIZMOGROUPTYPE_PERSISTENT | WM_GIZMOGROUPTYPE_DRAW_MODAL_ALL;

  gzgt->poll = group_poll;
  gzgt->setup = group_setup;
  gzgt->invoke_prepare = gizmo_invoke_prepare;
  gzgt->setup_keymap = WM_gizmogroup_setup_keymap_generic_maybe_drag;
  gzgt->refresh = group_refresh;
  gzgt->draw_prepare = group_draw_prepare;
}

}  // namespace blender
