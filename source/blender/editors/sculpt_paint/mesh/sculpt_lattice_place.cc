/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Sculpt Lattice Tool: cage placement phase.
 *
 * Holds the phase transitions and the operators that only make sense while the cage is being
 * placed. Kept apart from sculpt_lattice.cc, which already owns the session lifecycle, the
 * screen-space pick, the slide modal and the RNA definitions.
 */

#include "sculpt_lattice.hh"
#include "sculpt_lattice_intern.hh"

#include "sculpt_intern.hh"

#include "MEM_guardedalloc.h"

#include "BKE_context.hh"
#include "BKE_lattice.hh"
#include "BKE_lib_id.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_report.hh"

#include "BLI_bounds.hh"
#include "BLI_bounds_types.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_curve_types.h" /* For #BPoint, the full type behind #Lattice.def. */
#include "DNA_lattice_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include <cmath>
#include <optional>

namespace blender::ed::sculpt_paint::lattice {

/* -------------------------------------------------------------------- */
/** \name Cage reset
 * \{ */

/**
 * Puts every control point back on the neutral grid, making the lattice deformation the identity.
 *
 * The grid origin and step are read straight from #Lattice.fu / #Lattice.du and friends rather
 * than recomputed with #calc_lat_fudu. #BKE_lattice_resize overrides those values to span
 * [-0.5, 0.5] whenever it is given an object — which is how this tool creates its cage — and
 * then stores the overridden values in the DNA fields. Recomputing would produce a different
 * grid and leave the cage permanently "deformed". Reading the DNA fields is correct by
 * construction: #BKE_lattice_deform_data_create subtracts exactly these values, so the
 * resulting offsets are identically zero.
 */
static void sculpt_lattice_cage_reset_to_neutral(Lattice &lt)
{
  if (lt.def == nullptr) {
    return;
  }
  BPoint *bp = lt.def;
  for (const int w : IndexRange(lt.pntsw)) {
    for (const int v : IndexRange(lt.pntsv)) {
      for (const int u : IndexRange(lt.pntsu)) {
        bp->vec[0] = lt.fu + float(u) * lt.du;
        bp->vec[1] = lt.fv + float(v) * lt.dv;
        bp->vec[2] = lt.fw + float(w) * lt.dw;
        bp++;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Phase transitions
 * \{ */

void sculpt_lattice_enter_placement(Object & /*ob_mesh*/, LatticeToolData &state)
{
  AffectedRegion &ar = state.current;

  /* Bake: the positions this tool last wrote become the new rest, so resetting the cage below
   * leaves the mesh exactly where it is. */
  if (!ar.current_coords.is_empty()) {
    ar.rest_coords.reinitialize(ar.current_coords.size());
    ar.rest_coords.as_mutable_span().copy_from(ar.current_coords);
  }

  if (state.lattice_ob != nullptr) {
    if (Lattice *lt = id_cast<Lattice *>(state.lattice_ob->data)) {
      sculpt_lattice_cage_reset_to_neutral(*lt);
    }
  }

  /* The deform context caches offsets computed from the control points that were just reset.
   * Every current consumer rebuilds it before use, but that invariant is implicit and would break
   * with the first consumer that forgets, so drop it here. */
  if (state.deform_data != nullptr) {
    BKE_lattice_deform_data_destroy(state.deform_data);
    state.deform_data = nullptr;
  }

  state.phase = Phase::Placement;
}

bool sculpt_lattice_enter_deform(bContext *C, Object &ob_mesh, LatticeToolData &state)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  if (depsgraph == nullptr || sculpt_lattice_pbvh_ensure(*depsgraph, ob_mesh) == nullptr) {
    return false;
  }

  /* Re-snapshot against the mesh as it is now. Correct here precisely because the cage is neutral:
   * nothing of the previous deformation is still encoded in the control points. */
  sculpt_lattice_build_affected_region(*depsgraph, ob_mesh, state);

  if (state.current.verts.is_empty()) {
    BKE_report(CTX_wm_reports(C),
               RPT_WARNING,
               "No deformable vertices inside the cage; move or resize it first");
    return false;
  }

  state.phase = Phase::Deform;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Box-defined cage geometry
 * \{ */

constexpr float SCULPT_LATTICE_BOX_MIN_PX = 5.0f;
constexpr float SCULPT_LATTICE_BOX_MIN_THICKNESS = 1e-4f;

Bounds<float2> sculpt_lattice_box_local_rect(const float2 local_delta,
                                             const bool origin_center,
                                             const bool fixed_aspect)
{
  float2 delta = local_delta;
  if (fixed_aspect) {
    delta.y = std::copysign(math::abs(delta.x), delta.y);
  }
  if (origin_center) {
    return {math::min(-delta, delta), math::max(-delta, delta)};
  }
  const float2 zero(0.0f);
  return {math::min(zero, delta), math::max(zero, delta)};
}

LatticeBoxTransform sculpt_lattice_box_transform_from_rect(const float3 &plane_origin,
                                                           const float3x3 &basis,
                                                           const Bounds<float2> &rect,
                                                           const float3 &translation,
                                                           const float thickness,
                                                           const bool flip)
{
  const float2 size2 = math::abs(rect.size());
  const float2 center2 = rect.center();
  const float depth = math::max(thickness, SCULPT_LATTICE_BOX_MIN_THICKNESS);

  LatticeBoxTransform xf;
  xf.scale = float3(math::max(size2.x, SCULPT_LATTICE_BOX_MIN_THICKNESS),
                    math::max(size2.y, SCULPT_LATTICE_BOX_MIN_THICKNESS),
                    depth);
  xf.rotation = basis;
  xf.front_center = plane_origin + translation + basis[0] * center2.x + basis[1] * center2.y;
  xf.extrude_dir = flip ? basis[2] : -basis[2];
  xf.location = xf.front_center + xf.extrude_dir * (depth * 0.5f);
  return xf;
}

std::optional<float> sculpt_lattice_box_thickness_from_lines(const float3 &front_center,
                                                             const float3 &extrude_dir,
                                                             const float3 &ray_a,
                                                             const float3 &ray_b,
                                                             const float min_thickness)
{
  const float3 dir = math::normalize(extrude_dir);
  if (UNLIKELY(math::length_squared(dir) < 1e-12f)) {
    return std::nullopt;
  }
  float3 i1;
  float3 i2;
  const int count = math::isect_line_line(front_center, front_center + dir, ray_a, ray_b, i1, i2);
  if (count == 0) {
    return std::nullopt;
  }
  return math::max(math::dot(i1 - front_center, dir), min_thickness);
}

/** Transient state of the two-stage #SCULPT_OT_lattice_box_define modal. */
struct BoxDefineData {
  enum class Stage {
    Rect = 0,
    Depth,
    Move,
  };
  Stage stage = Stage::Rect;
  Stage stage_before_move = Stage::Rect;

  /** Workplane: origin under the click, Z = plane normal, X/Y the drawing axes. */
  float3 plane_origin = float3(0.0f);
  float3x3 basis = float3x3::identity();
  float4 plane = float4(0.0f, 0.0f, 1.0f, 0.0f);

  /** Screen-space click vs current mouse, used for the minimum-drag guard. */
  float2 mval_start = float2(0.0f);
  float2 mval_current = float2(0.0f);

  Bounds<float2> rect = {float2(0.0f), float2(0.0f)};
  float3 translation = float3(0.0f);

  bool origin_center = false;
  bool fixed_aspect = false;
  bool flip = false;
  bool surface_aligned = false;

  float thickness = 1.0f;
  float thickness_initial = 1.0f;

  float3 move_grab_world = float3(0.0f);
  float3 translation_at_grab = float3(0.0f);
};

static float3x3 sculpt_lattice_box_basis_from_normal(const float3 &normal,
                                                     const float3 &view_x,
                                                     const float3 &view_y)
{
  const float3 z = math::normalize(normal);
  float3 x = view_x - z * math::dot(view_x, z);
  if (math::length_squared(x) < 1e-10f) {
    x = view_y - z * math::dot(view_y, z);
  }
  x = math::normalize(x);
  const float3 y = math::normalize(math::cross(z, x));
  x = math::cross(y, z);
  float3x3 basis;
  basis[0] = x;
  basis[1] = y;
  basis[2] = z;
  return basis;
}

static float sculpt_lattice_box_bounds_thickness(const Object &ob_mesh,
                                                 const Bounds<float3> &bounds_mesh_space,
                                                 const float3 &axis)
{
  const float4x4 &to_world = ob_mesh.object_to_world();
  const float3 &bmin = bounds_mesh_space.min;
  const float3 &bmax = bounds_mesh_space.max;
  const float3 dir = math::normalize(axis);

  float depth_min = FLT_MAX;
  float depth_max = -FLT_MAX;
  for (const int i : IndexRange(8)) {
    const float3 corner_local((i & 1) ? bmax.x : bmin.x,
                              (i & 2) ? bmax.y : bmin.y,
                              (i & 4) ? bmax.z : bmin.z);
    const float3 corner_world = math::transform_point(to_world, corner_local);
    const float depth = math::dot(corner_world, dir);
    depth_min = math::min(depth_min, depth);
    depth_max = math::max(depth_max, depth);
  }
  return math::max(depth_max - depth_min, SCULPT_LATTICE_BOX_MIN_THICKNESS);
}

static bool sculpt_lattice_box_project_mval(const ARegion *region,
                                            const BoxDefineData &box,
                                            const float2 &mval,
                                            float3 &r_world)
{
  const float4 fallback = math::plane_from_point_normal(box.plane_origin, box.basis[2]);
  return ED_view3d_win_to_3d_on_plane_with_fallback(
      region, box.plane, mval, false, fallback, r_world);
}

static LatticeBoxTransform sculpt_lattice_box_current_transform(const BoxDefineData &box)
{
  return sculpt_lattice_box_transform_from_rect(
      box.plane_origin, box.basis, box.rect, box.translation, box.thickness, box.flip);
}

static void sculpt_lattice_box_apply(const BoxDefineData &box, Object &lat_ob)
{
  const LatticeBoxTransform xf = sculpt_lattice_box_current_transform(box);

  copy_v3_v3(lat_ob.loc, xf.location);
  copy_v3_v3(lat_ob.scale, xf.scale);
  float3x3 rotation = xf.rotation;
  BKE_object_mat3_to_rot(&lat_ob, rotation.ptr(), false);

  /* ADR-15: the cage is in no depsgraph, so its runtime matrix has to be refreshed by hand. */
  BKE_object_to_mat4(&lat_ob, lat_ob.runtime->object_to_world.ptr());
}

static bool sculpt_lattice_box_update_rect(const ARegion *region,
                                           BoxDefineData &box,
                                           const float2 &mval)
{
  float3 world;
  if (!sculpt_lattice_box_project_mval(region, box, mval, world)) {
    return false;
  }
  box.mval_current = mval;
  const float3 rel = world - (box.plane_origin + box.translation);
  const float2 delta(math::dot(rel, box.basis[0]), math::dot(rel, box.basis[1]));
  box.rect = sculpt_lattice_box_local_rect(delta, box.origin_center, box.fixed_aspect);
  return true;
}

static void sculpt_lattice_box_init_view_workplane(const Object &ob_mesh,
                                                   const Bounds<float3> &bounds_mesh_space,
                                                   const RegionView3D &rv3d,
                                                   const ARegion *region,
                                                   const float2 &mval,
                                                   BoxDefineData &box)
{
  box.basis = float3x3(float4x4(rv3d.viewinv));
  for (const int axis : IndexRange(3)) {
    box.basis[axis] = math::normalize(box.basis[axis]);
  }

  const float3 depth_ref = math::transform_point(ob_mesh.object_to_world(),
                                                 bounds_mesh_space.center());
  box.plane = math::plane_from_point_normal(depth_ref, box.basis[2]);
  float3 origin = depth_ref;
  sculpt_lattice_box_project_mval(region, box, mval, origin);
  box.plane_origin = origin;
  box.plane = math::plane_from_point_normal(box.plane_origin, box.basis[2]);
  box.surface_aligned = false;
  box.thickness_initial = sculpt_lattice_box_bounds_thickness(ob_mesh, bounds_mesh_space, box.basis[2]);
  box.thickness = box.thickness_initial;
}

static bool sculpt_lattice_box_try_surface_workplane(bContext *C,
                                                     const Object &ob_mesh,
                                                     const Bounds<float3> &bounds_mesh_space,
                                                     const RegionView3D &rv3d,
                                                     const float2 &mval,
                                                     BoxDefineData &box)
{
  const Paint *paint = BKE_paint_get_active_from_context(C);
  if (paint == nullptr || BKE_paint_brush_for_read(paint) == nullptr) {
    return false;
  }

  const std::optional<CursorGeometryInfo> cgi = cursor_geometry_info_update(C, mval, false);
  if (!cgi.has_value() || math::length_squared(cgi->normal) < 1e-12f) {
    return false;
  }

  const float4x4 &to_world = ob_mesh.object_to_world();
  const float3 hit_world = math::transform_point(to_world, cgi->location);
  const float3 normal_world = math::normalize(math::transform_direction(to_world, cgi->normal));
  const float3x3 view_basis = float3x3(float4x4(rv3d.viewinv));
  const float3 view_x = math::normalize(view_basis[0]);
  const float3 view_y = math::normalize(view_basis[1]);

  box.basis = sculpt_lattice_box_basis_from_normal(normal_world, view_x, view_y);
  box.plane_origin = hit_world;
  box.plane = math::plane_from_point_normal(box.plane_origin, box.basis[2]);
  box.surface_aligned = true;
  box.thickness_initial = sculpt_lattice_box_bounds_thickness(ob_mesh, bounds_mesh_space, box.basis[2]);
  box.thickness = box.thickness_initial;
  return true;
}

static bool sculpt_lattice_box_is_navigation_event(const wmEvent *event)
{
  return ELEM(event->type,
              MIDDLEMOUSE,
              MOUSEPAN,
              MOUSEZOOM,
              MOUSEROTATE,
              WHEELUPMOUSE,
              WHEELDOWNMOUSE,
              NDOF_MOTION);
}

static void sculpt_lattice_box_status(bContext *C, const BoxDefineData &box)
{
  WorkspaceStatus status(C);
  const BoxDefineData::Stage stage = (box.stage == BoxDefineData::Stage::Move) ?
                                         box.stage_before_move :
                                         box.stage;
  if (box.stage == BoxDefineData::Stage::Move) {
    status.item(IFACE_("Move"), ICON_MOUSE_MOVE, ICON_EVENT_SPACEKEY);
    status.item(IFACE_("Confirm"), ICON_MOUSE_LMB);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
    return;
  }
  if (stage == BoxDefineData::Stage::Rect) {
    status.item(IFACE_("Draw"), ICON_MOUSE_MOVE);
    status.item(IFACE_("Confirm"), ICON_MOUSE_LMB);
    status.item_bool(IFACE_("Square"), box.fixed_aspect, ICON_EVENT_SHIFT);
    status.item_bool(IFACE_("From Center"), box.origin_center, ICON_EVENT_ALT);
    status.item(IFACE_("Move"), ICON_EVENT_SPACEKEY);
    status.item(box.surface_aligned ? IFACE_("Surface") : IFACE_("View"), 0);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
  }
  else {
    status.item(IFACE_("Set Depth"), ICON_MOUSE_MOVE);
    status.item(IFACE_("Confirm / Edit Points"), ICON_MOUSE_LMB);
    status.item_bool(IFACE_("Flip"), box.flip, ICON_EVENT_F);
    status.item(IFACE_("Move"), ICON_EVENT_SPACEKEY);
    status.item(IFACE_("Back"), ICON_EVENT_ESC);
  }
}

void sculpt_lattice_status_idle(bContext *C)
{
  Object *ob_mesh = CTX_data_active_object(C);
  const LatticeToolData *state = nullptr;
  if (ob_mesh != nullptr && ob_mesh->runtime->sculpt_session != nullptr) {
    state = ob_mesh->runtime->sculpt_session->lattice_tool_state;
  }

  WorkspaceStatus status(C);
  if (state != nullptr && state->phase == Phase::Deform) {
    status.item(IFACE_("Drag Point"), ICON_MOUSE_LMB);
    status.item(IFACE_("Place Cage"), ICON_EVENT_C);
    status.item(IFACE_("Confirm"), ICON_EVENT_RETURN);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
    return;
  }

  status.item(IFACE_("Draw Box"), ICON_MOUSE_LMB);
  status.item(IFACE_("Move"), ICON_EVENT_G);
  status.item(IFACE_("Rotate"), ICON_EVENT_R);
  status.item(IFACE_("Scale"), ICON_EVENT_S);
  status.item(IFACE_("Fit"), ICON_EVENT_F);
  status.item(IFACE_("Edit Points"), ICON_EVENT_C);
  status.item(IFACE_("Confirm"), ICON_EVENT_RETURN);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SCULPT_OT_lattice_phase_toggle
 * \{ */

static wmOperatorStatus sculpt_lattice_phase_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  if (!sculpt_lattice_ensure_session(C)) {
    return OPERATOR_CANCELLED;
  }
  Object *ob_mesh = CTX_data_active_object(C);
  if (ob_mesh == nullptr || ob_mesh->runtime->sculpt_session == nullptr) {
    return OPERATOR_CANCELLED;
  }
  LatticeToolData *state = ob_mesh->runtime->sculpt_session->lattice_tool_state;
  if (state == nullptr) {
    return OPERATOR_CANCELLED;
  }

  if (state->phase == Phase::Deform) {
    sculpt_lattice_enter_placement(*ob_mesh, *state);
  }
  else if (!sculpt_lattice_enter_deform(C, *ob_mesh, *state)) {
    /* Stay in placement so the user can fix the cage; the report explains why. */
    return OPERATOR_CANCELLED;
  }

  /* ADR-15: the no-main cage is in no depsgraph, and neither transition moves a vertex, so a
   * redraw is all that is needed for the overlay to pick up the new phase. */
  ED_region_tag_redraw(CTX_wm_region(C));
  sculpt_lattice_status_idle(C);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_lattice_phase_toggle(wmOperatorType *ot)
{
  ot->name = "Lattice Toggle Phase";
  ot->idname = "SCULPT_OT_lattice_phase_toggle";
  ot->description = "Switch between placing the lattice cage and deforming with it";

  ot->exec = sculpt_lattice_phase_toggle_exec;
  ot->poll = sculpt_lattice_tool_active_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SCULPT_OT_lattice_transform
 * \{ */

/**
 * Ensures a session exists, then hands over to the stock transform operator.
 *
 * The transform system cannot create the tool session itself, and #TransConvertType_SculptLattice
 * is only selected when one already exists in the placement phase. Wrapping the call is the same
 * trick #sculpt_lattice_pick_invoke uses to reach #SCULPT_OT_lattice_slide.
 */
static wmOperatorStatus sculpt_lattice_transform_invoke(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event)
{
  if (!sculpt_lattice_ensure_session(C)) {
    return OPERATOR_CANCELLED;
  }
  Object *ob_mesh = CTX_data_active_object(C);
  if (ob_mesh == nullptr || ob_mesh->runtime->sculpt_session == nullptr) {
    return OPERATOR_CANCELLED;
  }
  LatticeToolData *state = ob_mesh->runtime->sculpt_session->lattice_tool_state;
  if (state == nullptr || state->phase != Phase::Placement) {
    BKE_report(CTX_wm_reports(C), RPT_INFO, "Press C to switch to lattice placement");
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  /* #TransConvertType_SculptLattice::create_trans_data (createTransSculptLattice) reaches
   * #undo::push_begin_ex, which dereferences the mesh's PBVH with no null check, and this operator
   * can be invoked again after a geometry re-eval happened between invocations. Prepare the object
   * here, the same way #sculpt_paint::init_transform does before its own #undo::push_begin_ex
   * call, so the stock transform operator never sees a stale-or-freed PBVH. */
  if (depsgraph == nullptr || sculpt_lattice_pbvh_ensure(*depsgraph, *ob_mesh) == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const char *transform_idname;
  switch (RNA_enum_get(op->ptr, "mode")) {
    case SCULPT_LATTICE_XFORM_ROTATE:
      transform_idname = "TRANSFORM_OT_rotate";
      break;
    case SCULPT_LATTICE_XFORM_RESIZE:
      transform_idname = "TRANSFORM_OT_resize";
      break;
    default:
      transform_idname = "TRANSFORM_OT_translate";
      break;
  }

  const wmOperatorStatus result = WM_operator_name_call(
      C, transform_idname, wm::OpCallContext::InvokeDefault, nullptr, event);
  if (result != OPERATOR_RUNNING_MODAL) {
    sculpt_lattice_status_idle(C);
  }
  return result;
}

void SCULPT_OT_lattice_transform(wmOperatorType *ot)
{
  ot->name = "Lattice Transform Cage";
  ot->idname = "SCULPT_OT_lattice_transform";
  ot->description = "Move, rotate or scale the lattice cage";

  ot->invoke = sculpt_lattice_transform_invoke;
  ot->poll = sculpt_lattice_tool_active_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_DEPENDS_ON_CURSOR;

  static const EnumPropertyItem mode_items[] = {
      {SCULPT_LATTICE_XFORM_TRANSLATE, "TRANSLATE", 0, "Move", "Move the cage"},
      {SCULPT_LATTICE_XFORM_ROTATE, "ROTATE", 0, "Rotate", "Rotate the cage"},
      {SCULPT_LATTICE_XFORM_RESIZE, "RESIZE", 0, "Resize", "Scale the cage"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_enum(ot->srna,
               "mode",
               mode_items,
               SCULPT_LATTICE_XFORM_TRANSLATE,
               "Mode",
               "Which transformation to start");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SCULPT_OT_lattice_fit
 * \{ */

static wmOperatorStatus sculpt_lattice_fit_exec(bContext *C, wmOperator * /*op*/)
{
  if (!sculpt_lattice_ensure_session(C)) {
    return OPERATOR_CANCELLED;
  }
  Object *ob_mesh = CTX_data_active_object(C);
  if (ob_mesh == nullptr || ob_mesh->runtime->sculpt_session == nullptr) {
    return OPERATOR_CANCELLED;
  }
  LatticeToolData *state = ob_mesh->runtime->sculpt_session->lattice_tool_state;
  if (state == nullptr || state->lattice_ob == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (state->phase != Phase::Placement) {
    BKE_report(CTX_wm_reports(C), RPT_INFO, "Press C to switch to lattice placement");
    return OPERATOR_CANCELLED;
  }

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  /* #sculpt_lattice_compute_deform_bounds reads the PBVH but cannot prepare it itself. */
  if (depsgraph == nullptr || sculpt_lattice_pbvh_ensure(*depsgraph, *ob_mesh) == nullptr) {
    return OPERATOR_CANCELLED;
  }

  std::optional<Bounds<float3>> bounds;
  if (!sculpt_lattice_compute_deform_bounds(*depsgraph, *ob_mesh, state->mask_eps, bounds)) {
    /* Leave the cage alone: the user may have placed it deliberately over an area whose mask they
     * are about to lift, and silently resetting it would throw that away. */
    BKE_report(CTX_wm_reports(C), RPT_WARNING, "All vertices are masked; the cage was left as is");
    return OPERATOR_CANCELLED;
  }

  sculpt_lattice_fit_temp_to_bounds(*state->lattice_ob, *ob_mesh, *bounds, state->margin);

  ED_region_tag_redraw(CTX_wm_region(C));
  sculpt_lattice_status_idle(C);
  return OPERATOR_FINISHED;
}

void SCULPT_OT_lattice_fit(wmOperatorType *ot)
{
  ot->name = "Lattice Fit to Unmasked";
  ot->idname = "SCULPT_OT_lattice_fit";
  ot->description = "Fit the lattice cage to the unmasked part of the mesh";

  ot->exec = sculpt_lattice_fit_exec;
  ot->poll = sculpt_lattice_tool_active_poll;

  ot->flag = OPTYPE_REGISTER;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name SCULPT_OT_lattice_box_define
 * \{ */

/** \a r_ob_mesh may be null when the caller only needs the state. */
static LatticeToolData *sculpt_lattice_state_for_placement(bContext *C, Object **r_ob_mesh)
{
  Object *ob_mesh = CTX_data_active_object(C);
  if (ob_mesh == nullptr || ob_mesh->runtime->sculpt_session == nullptr) {
    return nullptr;
  }
  LatticeToolData *state = ob_mesh->runtime->sculpt_session->lattice_tool_state;
  if (state == nullptr || state->lattice_ob == nullptr || state->phase != Phase::Placement) {
    return nullptr;
  }
  if (r_ob_mesh != nullptr) {
    *r_ob_mesh = ob_mesh;
  }
  return state;
}

static wmOperatorStatus sculpt_lattice_box_define_invoke(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  if (!sculpt_lattice_ensure_session(C)) {
    return OPERATOR_CANCELLED;
  }
  Object *ob_mesh = nullptr;
  LatticeToolData *state = sculpt_lattice_state_for_placement(C, &ob_mesh);
  if (state == nullptr) {
    return OPERATOR_CANCELLED;
  }
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  /* #sculpt_lattice_compute_deform_bounds reads the PBVH but cannot prepare it itself. */
  if (region == nullptr || rv3d == nullptr || depsgraph == nullptr ||
      sculpt_lattice_pbvh_ensure(*depsgraph, *ob_mesh) == nullptr)
  {
    return OPERATOR_CANCELLED;
  }

  std::optional<Bounds<float3>> bounds;
  if (!sculpt_lattice_compute_deform_bounds(*depsgraph, *ob_mesh, state->mask_eps, bounds)) {
    const Span<float3> all_positions = bke::pbvh::vert_positions_eval(*depsgraph, *ob_mesh);
    bounds = bounds::min_max(all_positions);
    if (!bounds.has_value()) {
      return OPERATOR_CANCELLED;
    }
  }

  BoxDefineData *box = MEM_new<BoxDefineData>(__func__);
  const float2 mval(float(event->mval[0]), float(event->mval[1]));
  box->mval_start = mval;
  box->mval_current = mval;
  box->origin_center = (event->modifier & KM_ALT) != 0;
  box->fixed_aspect = (event->modifier & KM_SHIFT) != 0;

  if (!sculpt_lattice_box_try_surface_workplane(C, *ob_mesh, *bounds, *rv3d, mval, *box)) {
    sculpt_lattice_box_init_view_workplane(*ob_mesh, *bounds, *rv3d, region, mval, *box);
  }

  op->customdata = box;

  WM_event_add_modal_handler(C, op);
  sculpt_lattice_box_status(C, *box);
  ED_region_tag_redraw(region);
  return OPERATOR_RUNNING_MODAL;
}

static void sculpt_lattice_box_define_free(wmOperator *op)
{
  if (op->customdata != nullptr) {
    MEM_delete(static_cast<BoxDefineData *>(op->customdata));
    op->customdata = nullptr;
  }
}

static wmOperatorStatus sculpt_lattice_box_define_modal(bContext *C,
                                                        wmOperator *op,
                                                        const wmEvent *event)
{
  Object *ob_mesh = nullptr;
  LatticeToolData *state = sculpt_lattice_state_for_placement(C, &ob_mesh);
  BoxDefineData *box = static_cast<BoxDefineData *>(op->customdata);
  ARegion *region = CTX_wm_region(C);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (state == nullptr || box == nullptr || region == nullptr || rv3d == nullptr) {
    sculpt_lattice_box_define_free(op);
    sculpt_lattice_status_idle(C);
    return OPERATOR_CANCELLED;
  }

  const float2 mval(float(event->mval[0]), float(event->mval[1]));
  box->origin_center = (event->modifier & KM_ALT) != 0;
  box->fixed_aspect = (event->modifier & KM_SHIFT) != 0;

  auto finish_ok = [&]() {
    sculpt_lattice_box_apply(*box, *state->lattice_ob);
    /* Box is placed: switch to deform so LMB immediately edits control points. */
    if (ob_mesh != nullptr) {
      sculpt_lattice_enter_deform(C, *ob_mesh, *state);
    }
    sculpt_lattice_box_define_free(op);
    ED_region_tag_redraw(region);
    sculpt_lattice_status_idle(C);
    return OPERATOR_FINISHED;
  };
  auto finish_cancel = [&]() {
    sculpt_lattice_box_define_free(op);
    sculpt_lattice_status_idle(C);
    return OPERATOR_CANCELLED;
  };

  if (event->type == EVT_SPACEKEY && event->val == KM_PRESS &&
      box->stage != BoxDefineData::Stage::Move)
  {
    float3 grab_world;
    if (sculpt_lattice_box_project_mval(region, *box, mval, grab_world)) {
      box->stage_before_move = box->stage;
      box->stage = BoxDefineData::Stage::Move;
      box->move_grab_world = grab_world;
      box->translation_at_grab = box->translation;
    }
    sculpt_lattice_box_status(C, *box);
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == EVT_SPACEKEY && event->val == KM_RELEASE &&
      box->stage == BoxDefineData::Stage::Move)
  {
    box->stage = box->stage_before_move;
    sculpt_lattice_box_status(C, *box);
    return OPERATOR_RUNNING_MODAL;
  }

  if (box->stage == BoxDefineData::Stage::Move) {
    if (event->type == MOUSEMOVE) {
      float3 current_world;
      if (sculpt_lattice_box_project_mval(region, *box, mval, current_world)) {
        box->translation = box->translation_at_grab + (current_world - box->move_grab_world);
        sculpt_lattice_box_apply(*box, *state->lattice_ob);
        ED_region_tag_redraw(region);
      }
      sculpt_lattice_box_status(C, *box);
      return OPERATOR_RUNNING_MODAL;
    }
    if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
      box->translation = box->translation_at_grab;
      box->stage = box->stage_before_move;
      sculpt_lattice_box_apply(*box, *state->lattice_ob);
      sculpt_lattice_box_status(C, *box);
      ED_region_tag_redraw(region);
      return OPERATOR_RUNNING_MODAL;
    }
    if (sculpt_lattice_box_is_navigation_event(event)) {
      return OPERATOR_PASS_THROUGH;
    }
    sculpt_lattice_box_status(C, *box);
    return OPERATOR_RUNNING_MODAL;
  }

  switch (box->stage) {
    case BoxDefineData::Stage::Rect: {
      const bool modifiers_changed = (event->type == EVT_LEFTSHIFTKEY ||
                                      event->type == EVT_RIGHTSHIFTKEY ||
                                      event->type == EVT_LEFTALTKEY ||
                                      event->type == EVT_RIGHTALTKEY);
      if (event->type == MOUSEMOVE || modifiers_changed) {
        if (sculpt_lattice_box_update_rect(region, *box, mval)) {
          sculpt_lattice_box_apply(*box, *state->lattice_ob);
          ED_region_tag_redraw(region);
        }
        sculpt_lattice_box_status(C, *box);
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
        /* Re-anchor. Esc from #Stage::Depth lands back here with the mouse already up and
         * the previous click still pinned, so without this the rectangle would keep
         * rubber-banding off that stale corner with no button held. */
        box->mval_start = mval;
        box->mval_current = mval;
        box->translation = float3(0.0f);
        float3 origin;
        if (sculpt_lattice_box_project_mval(region, *box, mval, origin)) {
          box->plane_origin = origin;
          box->plane = math::plane_from_point_normal(box->plane_origin, box->basis[2]);
        }
        box->rect = {float2(0.0f), float2(0.0f)};
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {
        const float2 size = math::abs(box->mval_current - box->mval_start);
        if (size.x < SCULPT_LATTICE_BOX_MIN_PX || size.y < SCULPT_LATTICE_BOX_MIN_PX) {
          return finish_cancel();
        }
        box->stage = BoxDefineData::Stage::Depth;
        sculpt_lattice_box_status(C, *box);
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
        return finish_cancel();
      }
      /* Pass through navigation events only (list copied from the non-modal-mapped switch in
       * #knifetool_modal, editmesh_knife.cc, plus #MIDDLEMOUSE since this modal has no modal
       * keymap of its own to route middle-mouse panning through first). Every other event must
       * stay `OPERATOR_RUNNING_MODAL`: the tool keymap this operator was invoked from binds
       * C/F/G/R/S/Enter, and letting any of those reach it while this modal is running would start
       * a second operator on top of this one's own drag.
       *
       * The pass-through itself must stay bare, not
       * `OPERATOR_PASS_THROUGH | OPERATOR_RUNNING_MODAL`: the combined flags still set
       * `WM_HANDLER_BREAK` in #wm_handler_operator_call, which stops
       * #wm_event_do_handlers from ever reaching area/region handling and keeps blocking view
       * navigation. A bare pass-through maps to `WM_HANDLER_CONTINUE` instead, so MMB orbit, wheel
       * zoom and NDOF motion reach the view keymap while this handler stays registered, since
       * removal only happens on `OPERATOR_CANCELLED | OPERATOR_FINISHED`. */
      if (sculpt_lattice_box_is_navigation_event(event)) {
        return OPERATOR_PASS_THROUGH;
      }
      sculpt_lattice_box_status(C, *box);
      return OPERATOR_RUNNING_MODAL;
    }
    case BoxDefineData::Stage::Depth: {
      if (event->type == EVT_FKEY && event->val == KM_PRESS) {
        box->flip = !box->flip;
        box->thickness = box->thickness_initial;
        sculpt_lattice_box_apply(*box, *state->lattice_ob);
        sculpt_lattice_box_status(C, *box);
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->type == MOUSEMOVE) {
        float3 ray_start;
        float3 ray_normal;
        ED_view3d_win_to_ray(region, mval, ray_start, ray_normal);
        const LatticeBoxTransform xf = sculpt_lattice_box_current_transform(*box);
        if (const std::optional<float> thickness = sculpt_lattice_box_thickness_from_lines(
                xf.front_center,
                xf.extrude_dir,
                ray_start,
                ray_start + ray_normal,
                SCULPT_LATTICE_BOX_MIN_THICKNESS))
        {
          box->thickness = *thickness;
          sculpt_lattice_box_apply(*box, *state->lattice_ob);
          ED_region_tag_redraw(region);
        }
        sculpt_lattice_box_status(C, *box);
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
        return finish_ok();
      }
      if (event->type == EVT_ESCKEY && event->val == KM_PRESS) {
        /* Back to the rectangle stage rather than out of the operator: the rectangle is usually
         * right and only the depth needs another try. */
        box->stage = BoxDefineData::Stage::Rect;
        box->thickness = box->thickness_initial;
        sculpt_lattice_box_apply(*box, *state->lattice_ob);
        sculpt_lattice_box_status(C, *box);
        ED_region_tag_redraw(region);
        return OPERATOR_RUNNING_MODAL;
      }
      /* Same navigation-only allow-list and the same reason for staying bare as in #Stage::Rect
       * above. This window is not a drag: it lasts until the user clicks to confirm, so swallowing
       * every other event here would block navigation for as long as the user takes to judge the
       * depth, and letting a keymap event like G/R/S through would start a nested operator. */
      if (sculpt_lattice_box_is_navigation_event(event)) {
        return OPERATOR_PASS_THROUGH;
      }
      sculpt_lattice_box_status(C, *box);
      return OPERATOR_RUNNING_MODAL;
    }
    case BoxDefineData::Stage::Move: {
      break;
    }
  }
  sculpt_lattice_box_status(C, *box);
  return OPERATOR_RUNNING_MODAL;
}

static void sculpt_lattice_box_define_cancel(bContext *C, wmOperator *op)
{
  sculpt_lattice_box_define_free(op);
  sculpt_lattice_status_idle(C);
}

void SCULPT_OT_lattice_box_define(wmOperatorType *ot)
{
  ot->name = "Lattice Box Define";
  ot->idname = "SCULPT_OT_lattice_box_define";
  ot->description =
      "Draw a box on a view or surface plane to place the lattice cage, then drag to set its depth";

  ot->invoke = sculpt_lattice_box_define_invoke;
  ot->modal = sculpt_lattice_box_define_modal;
  ot->cancel = sculpt_lattice_box_define_cancel;
  ot->poll = sculpt_lattice_tool_active_poll;

  /* No #OPTYPE_UNDO on purpose. This operator only writes the cage's loc/rot/scale and never moves
   * a vertex — the cage is neutral throughout #Phase::Placement. Since no sculpt undo step is
   * opened here, the #OPTYPE_UNDO push at the end would fall through to the memfile undo type
   * (the sculpt type has no context poll and can never be selected automatically), reloading Main
   * and destroying the tool session. #SCULPT_OT_lattice_transform avoids the same trap by having
   * #createTransSculptLattice open a sculpt-typed step explicitly; a box redefine is cheap enough
   * to simply redo by hand instead. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_BLOCKING | OPTYPE_DEPENDS_ON_CURSOR;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::lattice
