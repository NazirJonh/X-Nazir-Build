/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 */

#include <algorithm>
#include <optional>

#include "DNA_brush_types.h"
#include "DNA_object_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_layer.hh"
#include "BKE_paint.hh"

#include "ED_paint.hh"
#include "ED_paint_curve_draw.hh"
#include "ED_screen.hh"
#include "ED_transform.hh"
#include "ED_view3d.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include "transform_mode.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Paint Curve Transform Data
 * \{ */

struct TransDataPaintCurve {
  /** Index into #PaintCurve::geometry points. */
  int point_index;
  /** 0 = left handle, 1 = pivot, 2 = right handle. */
  int handle_index;
  /** World-space position before transform; used as depth reference when unprojecting. */
  float co_orig_world[3];
  /** World-space position of the pivot (handle_index == 1) for this curve point.
   * Used as the local rotation center when individual origins is active. */
  float pivot_world[3];
  /**
   * Radius factor at the time the transform started.
   * `td->val` points here for #TFM_CURVE_SHRINKFATTEN so the system can modify it in-place.
   */
  float radius;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/* Matches the `paintcurve_selection` attribute defined in `paint_curve_geometry.cc`. */
static constexpr const char *PC_ATTR_SELECTION = "paintcurve_selection";

/** Fill one #TransData / #TransData2D entry for a single handle of a paint-curve point. */
static void paintcurve_fill_trans_entry(const int point_index,
                                        const int handle_index,
                                        const float screen_co[2],
                                        const float center_screen[2],
                                        const float world_co[3],
                                        const float pivot_world_co[3],
                                        const float point_radius,
                                        TransData2D *td2d,
                                        TransDataPaintCurve *tdpc,
                                        TransData *td)
{
  copy_v2_v2(td2d->loc, screen_co);
  td2d->loc[2] = 0.0f;
  /* Written back manually in #flushTransPaintCurve; skip generic 2D writeback. */
  td2d->loc2d = nullptr;

  td->flag = TD_SELECTED;
  td->loc = td2d->loc;
  td->center[0] = center_screen[0];
  td->center[1] = center_screen[1];
  td->center[2] = 0.0f;
  copy_v3_v3(td->iloc, td->loc);

  memset(td->axismtx, 0, sizeof(td->axismtx));
  td->axismtx[2][2] = 1.0f;

  unit_m3(td->mtx);
  unit_m3(td->smtx);

  td->dist = 0.0;

  tdpc->point_index = point_index;
  tdpc->handle_index = handle_index;
  tdpc->radius = point_radius;
  copy_v3_v3(tdpc->co_orig_world, world_co);
  copy_v3_v3(tdpc->pivot_world, pivot_world_co);

  /* Shrink-fatten operates on the pivot radius via `td->val`; other modes ignore it. */
  if (handle_index == 1) {
    td->val = &tdpc->radius;
    td->ival = point_radius;
  }
  else {
    td->val = nullptr;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Curve Transform Creation
 * \{ */

static void createTransPaintCurveVerts(bContext *C, TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  tc->data_len = 0;

  Paint *paint = BKE_paint_get_active_from_context(C);
  Brush *br = paint ? BKE_paint_brush(paint) : nullptr;
  if (!paint || !br) {
    return;
  }

  PaintCurve *pc = br->paint_curve;
  if (!pc) {
    return;
  }

  const bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime == nullptr || geom.points_num() == 0) {
    return;
  }

  const bool screen_space = !pc->use_3d_space;

  /* 3D paint curves live in object space; projection requires a 3D viewport. */
  if (!screen_space && (t->region == nullptr || t->spacetype != SPACE_VIEW3D)) {
    return;
  }

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  const Object *ob = BKE_view_layer_active_object_get(t->view_layer);
  const float (*ob_to_world)[4] = ob ? ob->object_to_world().ptr() : nullptr;

  /* Helpers: project an object-space point to 2D screen pixels. */
  auto project_to_screen = [&](const float3 &obj_co, float r_screen[2]) {
    if (screen_space) {
      r_screen[0] = obj_co.x;
      r_screen[1] = obj_co.y;
      return;
    }
    float world_co[3];
    if (ob_to_world) {
      mul_v3_m4v3(world_co, ob_to_world, obj_co);
    }
    else {
      copy_v3_v3(world_co, obj_co);
    }
    ED_view3d_project_v2(t->region, world_co, r_screen);
  };

  auto to_world = [&](const float3 &obj_co, float r_world[3]) {
    if (screen_space) {
      r_world[0] = obj_co.x;
      r_world[1] = obj_co.y;
      r_world[2] = 0.0f;
      return;
    }
    if (ob_to_world) {
      mul_v3_m4v3(r_world, ob_to_world, obj_co);
    }
    else {
      copy_v3_v3(r_world, obj_co);
    }
  };

  const Span<float3> positions = geom.positions();
  const std::optional<Span<float3>> handles_left = geom.handle_positions_left();
  const std::optional<Span<float3>> handles_right = geom.handle_positions_right();
  const VArray<float> radii = geom.radius();

  const bke::AttributeAccessor attrs = geom.attributes();
  const VArray<int8_t> sel_attr = *attrs.lookup_or_default<int8_t>(
      PC_ATTR_SELECTION, bke::AttrDomain::Point, int8_t(0));

  /* Count total handle entries that need transform data. */
  int total = 0;
  for (const int i : geom.points_range()) {
    const uint8_t sel = uint8_t(sel_attr[i]);
    if (!sel) {
      continue;
    }
    if (sel & 0x02) {
      total += 3; /* Pivot selected → left + pivot + right. */
    }
    else {
      if (sel & 0x01) total++;
      if (sel & 0x04) total++;
    }
  }

  if (!total) {
    return;
  }

  tc->data_len = total;
  TransData2D *td2d = tc->data_2d = MEM_new_array_zeroed<TransData2D>(total, "TransData2D");
  TransData *td = tc->data = MEM_new_array_zeroed<TransData>(total, "TransData");
  TransDataPaintCurve *tdpc = static_cast<TransDataPaintCurve *>(
      tc->custom.type.data = MEM_new_array_zeroed<TransDataPaintCurve>(
          total, "TransDataPaintCurve"));
  tc->custom.type.use_free = true;

  for (const int i : geom.points_range()) {
    const uint8_t sel = uint8_t(sel_attr[i]);
    if (!sel) {
      continue;
    }

    /* Gather the three handle positions in object space. */
    float3 obj_handles[3];
    obj_handles[1] = positions[i]; /* Pivot is always valid. */
    if (handles_left.has_value() && handles_right.has_value()) {
      obj_handles[0] = handles_left.value()[i];
      obj_handles[2] = handles_right.value()[i];
    }
    else {
      obj_handles[0] = obj_handles[1];
      obj_handles[2] = obj_handles[1];
    }

    /* Screen-space pivot — used as `td->center` for all three handles of this point. */
    float center_screen[2];
    project_to_screen(obj_handles[1], center_screen);

    /* World-space pivot — used as local rotation center for axis-constrained transforms. */
    float pivot_world[3];
    to_world(obj_handles[1], pivot_world);

    const float point_radius = radii ? max_ff(float(radii[i]), 0.0f) : 1.0f;

    if (sel & 0x02) {
      /* Pivot selected: add all three handles as independent entries. The transform
       * system applies the same screen-space delta to all, keeping relative offsets. */
      for (int j = 0; j < 3; j++) {
        float screen_co[2];
        float world_co[3];
        project_to_screen(obj_handles[j], screen_co);
        to_world(obj_handles[j], world_co);

        paintcurve_fill_trans_entry(
            i, j, screen_co, center_screen, world_co, pivot_world, point_radius, td2d, tdpc, td);
        td++;
        td2d++;
        tdpc++;
      }
    }
    else {
      if (sel & 0x01) {
        float screen_co[2];
        float world_co[3];
        project_to_screen(obj_handles[0], screen_co);
        to_world(obj_handles[0], world_co);
        paintcurve_fill_trans_entry(
            i, 0, screen_co, center_screen, world_co, pivot_world, point_radius, td2d, tdpc, td);
        td++;
        td2d++;
        tdpc++;
      }
      if (sel & 0x04) {
        float screen_co[2];
        float world_co[3];
        project_to_screen(obj_handles[2], screen_co);
        to_world(obj_handles[2], world_co);
        paintcurve_fill_trans_entry(
            i, 2, screen_co, center_screen, world_co, pivot_world, point_radius, td2d, tdpc, td);
        td++;
        td2d++;
        tdpc++;
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Paint Curve Transform Flush
 * \{ */

static void flushTransPaintCurve(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  if (tc->data_len == 0) {
    return;
  }

  TransData2D *td2d = tc->data_2d;
  TransDataPaintCurve *tdpc = static_cast<TransDataPaintCurve *>(tc->custom.type.data);
  const TransData *td_arr = tc->data;

  /* Resolve the active paint curve. */
  Paint *paint = t->context ? BKE_paint_get_active_from_context(t->context) :
                              BKE_paint_get_active(*t->bmain, t->scene, t->view_layer);
  Brush *br = paint ? BKE_paint_brush(paint) : nullptr;
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  if (!pc) {
    return;
  }

  bke::CurvesGeometry &geom = pc->geometry.wrap();
  if (geom.runtime == nullptr || geom.points_num() == 0) {
    return;
  }

  const bool screen_space = !pc->use_3d_space;

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  const Object *ob = BKE_view_layer_active_object_get(t->view_layer);
  const float4x4 world_to_obj = ob ? ob->world_to_object() : float4x4::identity();

  /* Unprojection requires a View3D pointer; only available in SPACE_VIEW3D. */
  View3D *v3d = (t->spacetype == SPACE_VIEW3D) ? static_cast<View3D *>(t->view) : nullptr;

  MutableSpan<float3> positions = geom.positions_for_write();
  MutableSpan<float3> handles_left = geom.handle_positions_left_for_write();
  MutableSpan<float3> handles_right = geom.handle_positions_right_for_write();

  const bool do_radius = (t->mode == TFM_CURVE_SHRINKFATTEN);
  MutableSpan<float> radii = do_radius ? geom.radius_for_write() : MutableSpan<float>{};

  for (int i = 0; i < tc->data_len; i++, td2d++, tdpc++) {
    const int pt = tdpc->point_index;
    const int hi = tdpc->handle_index;

    if (screen_space) {
      float3 new_obj;
      if (t->mode == TFM_ROTATION) {
        /* Recompute a pure 2D screen-plane rotation directly from the resolved angle instead of
         * trusting `td2d->loc` (as produced by the generic #ElementRotation_ex path). The
         * generic rotate mode picks its default (unconstrained) axis from the current Transform
         * Orientation matrix, which has no meaningful direction for flat region-pixel points, so
         * it does not reliably land on the screen normal. Deriving the rotation from the angle
         * value alone sidesteps that entirely and matches how the 3D-curve branch below already
         * does it for `has_axis_constraint == false`. */
        const TransData *td = &td_arr[i];
        const float angle = t->values_final[0];
        const float *center = transdata_check_local_center(t, t->around) ? td->center :
                                                                            t->center2d;
        const float dx = td->iloc[0] - center[0];
        const float dy = td->iloc[1] - center[1];
        const float cos_a = cosf(angle);
        const float sin_a = sinf(angle);
        new_obj = float3(
            center[0] + dx * cos_a - dy * sin_a, center[1] + dx * sin_a + dy * cos_a, 0.0f);
      }
      else {
        new_obj = float3(td2d->loc[0], td2d->loc[1], 0.0f);
      }
      switch (hi) {
        case 0:
          handles_left[pt] = new_obj;
          break;
        case 1:
          positions[pt] = new_obj;
          break;
        case 2:
          handles_right[pt] = new_obj;
          break;
        default:
          break;
      }
      if (do_radius && hi == 1) {
        radii[pt] = max_ff(tdpc->radius, 0.0f);
      }
      continue;
    }

    /* Unproject the new 2D screen position back to world space using the original
     * point depth as the reference plane. */
    float new_world[3];
    if (t->mode == TFM_ROTATION && v3d && t->region) {
      const bool has_axis_constraint = (t->con.mode & CON_APPLY) != 0;

      if (has_axis_constraint) {
        /* Axis-constrained rotation: apply a true 3D rotation in world space.
         * `ElementRotation_ex` would use screen-space trans-data which loses axis information,
         * so we compute the rotation directly from the constraint axis. */
        const float angle = t->values_final[0];

        /* Resolve the rotation axis in world space. Since t->spacemtx is kept in world space
         * during rotation, we can directly read the columns. */
        float axis[3] = {0.0f, 0.0f, 1.0f};
        {
          const int axis_mode = t->con.mode & (CON_AXIS0 | CON_AXIS1 | CON_AXIS2);
          switch (axis_mode) {
            case CON_AXIS0:
            case (CON_AXIS1 | CON_AXIS2):
              copy_v3_v3(axis, t->spacemtx[0]);
              break;
            case CON_AXIS1:
            case (CON_AXIS0 | CON_AXIS2):
              copy_v3_v3(axis, t->spacemtx[1]);
              break;
            case CON_AXIS2:
            case (CON_AXIS0 | CON_AXIS1):
            default:
              copy_v3_v3(axis, t->spacemtx[2]);
              break;
          }
          normalize_v3(axis);
        }

        float rot_mat[3][3];
        axis_angle_to_mat3(rot_mat, axis, angle);

        /* Determine the 3D world-space center of rotation. */
        float center_world[3];
        if (transdata_check_local_center(t, t->around)) {
          /* Individual origins: each point rotates around its own pivot. */
          copy_v3_v3(center_world, tdpc->pivot_world);
        }
        else {
          /* Median / bounding-box / cursor: use the centroid of all selected world positions. */
          zero_v3(center_world);
          const TransDataPaintCurve *tdpc_all =
              static_cast<const TransDataPaintCurve *>(tc->custom.type.data);
          for (int j = 0; j < tc->data_len; j++) {
            add_v3_v3(center_world, tdpc_all[j].co_orig_world);
          }
          mul_v3_fl(center_world, 1.0f / float(tc->data_len));
        }

        /* Rotate the original world position around the center. */
        float rel[3];
        sub_v3_v3v3(rel, tdpc->co_orig_world, center_world);
        mul_v3_m3v3(new_world, rot_mat, rel);
        add_v3_v3(new_world, center_world);
      }
      else {
        /* No axis constraint: compute a pure 2D screen-plane rotation, then unproject.
         * This matches the natural "rotate in view" behavior. */
        const TransData *td = &td_arr[i];
        const float angle = t->values_final[0];
        const float *center = transdata_check_local_center(t, t->around) ? td->center :
                                                                            t->center2d;
        const float dx = td->iloc[0] - center[0];
        const float dy = td->iloc[1] - center[1];
        const float cos_a = cosf(angle);
        const float sin_a = sinf(angle);
        float screen_pos[2];
        screen_pos[0] = center[0] + dx * cos_a - dy * sin_a;
        screen_pos[1] = center[1] + dx * sin_a + dy * cos_a;
        ED_view3d_win_to_3d(v3d, t->region, tdpc->co_orig_world, screen_pos, new_world);
      }
    }
    else if (t->mode == TFM_TRANSLATION) {
      /* 3D translation (constrained or free): apply the delta stored in t->values_final.
       * Since convertViewVec now returns world-space for paint curve translation, the
       * standard constraint pipeline produces t->values_final in orientation space.
       * Multiplying by t->spacemtx recovers the world-space delta — identical to how
       * applyTranslationMatrix works for regular objects. */
      float world_delta[3];
      mul_v3_m3v3(world_delta, t->spacemtx, t->values_final);

      /* axisProjection uses -factor in the near-parallel case (constraint axis nearly
       * parallel to view direction), which inverts the Z movement direction.
       * For Z-only constraint, detect this and flip the sign so that mouse-up always
       * moves the point in the +Z (world-up) direction. */
      if ((t->con.mode & CON_APPLY) &&
          (t->con.mode & CON_AXIS2) &&
          !(t->con.mode & (CON_AXIS0 | CON_AXIS1)))
      {
        float angle = fabsf(angle_v3v3(t->spacemtx[2], t->viewinv[2]));
        if (angle > float(M_PI_2)) {
          angle = float(M_PI) - angle;
        }
        if (angle < DEG2RADF(5.0f)) {
          world_delta[2] = -world_delta[2];
        }
      }

      add_v3_v3v3(new_world, tdpc->co_orig_world, world_delta);
    }
    else if (v3d && t->region) {
      ED_view3d_win_to_3d(v3d, t->region, tdpc->co_orig_world, td2d->loc, new_world);
    }
    else {
      copy_v3_v3(new_world, tdpc->co_orig_world);
    }

    /* Convert from world space to object space (paint curve stores object-space positions). */
    const float3 new_obj = math::transform_point(world_to_obj, float3(new_world));

    switch (hi) {
      case 0:
        handles_left[pt] = new_obj;
        break;
      case 1:
        positions[pt] = new_obj;
        break;
      case 2:
        handles_right[pt] = new_obj;
        break;
      default:
        break;
    }

    if (do_radius && hi == 1) {
      radii[pt] = max_ff(tdpc->radius, 0.0f);
    }
  }

  geom.calculate_bezier_auto_handles();
  geom.calculate_bezier_aligned_handles();
  geom.tag_positions_changed();

  if (t->context) {
    if (do_radius) {
      /* Clamps radii and syncs to source object. */
      ED_paintcurve_flush_radius_transform(t->context, pc);
    }
    else if (pc->use_3d_space) {
      ED_paintcurve_sync_to_source(t->context, pc);
    }
    if (br) {
      BKE_brush_tag_unsaved_changes(br);
    }

    /* Tag paint curve overlay and viewport for immediate redraw during transform.
     * Without this, the control points don't update visually until the transform completes. */
    sculpt_paint::ED_paint_curve_overlay_tag_redraw_all(t->context);
    if (t->region) {
      ED_region_tag_redraw(t->region);
    }
  }
}

/** \} */

bool paintcurve_transform_use_3d_viewport(const TransInfo *t)
{
  if ((t->options & CTX_PAINT_CURVE) == 0 || t->spacetype != SPACE_VIEW3D) {
    return false;
  }
  Paint *paint = t->context ? BKE_paint_get_active_from_context(t->context) :
                              BKE_paint_get_active(*t->bmain, t->scene, t->view_layer);
  Brush *br = paint ? BKE_paint_brush(paint) : nullptr;
  PaintCurve *pc = br ? br->paint_curve : nullptr;
  return pc && pc->use_3d_space;
}

bool paintcurve_trans_data_is_pivot(const TransDataContainer *tc, const int data_index)
{
  const TransDataPaintCurve *tdpc_arr = static_cast<const TransDataPaintCurve *>(
      tc->custom.type.data);
  return tdpc_arr[data_index].handle_index == 1;
}

void paintcurve_snap_source_world_get(const TransDataContainer *tc,
                                      const int data_index,
                                      float r_world[3])
{
  const TransDataPaintCurve *tdpc_arr = static_cast<const TransDataPaintCurve *>(
      tc->custom.type.data);
  copy_v3_v3(r_world, tdpc_arr[data_index].co_orig_world);
}

void paintcurve_center_median_3d_get(const TransInfo *t, float r_center[3])
{
  const TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  if (tc->data_len == 0) {
    zero_v3(r_center);
    return;
  }
  const TransDataPaintCurve *tdpc_arr = static_cast<const TransDataPaintCurve *>(tc->custom.type.data);
  zero_v3(r_center);
  for (int i = 0; i < tc->data_len; i++) {
    add_v3_v3(r_center, tdpc_arr[i].co_orig_world);
  }
  mul_v3_fl(r_center, 1.0f / float(tc->data_len));
}

TransConvertTypeInfo TransConvertType_PaintCurve = {
    /*flags*/ (T_POINTS | T_2D_EDIT),
    /*create_trans_data*/ createTransPaintCurveVerts,
    /*recalc_data*/ flushTransPaintCurve,
    /*special_aftertrans_update*/ nullptr,
};

}  // namespace blender::ed::transform
