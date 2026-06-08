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

  /* 3D paint curves live in object space; projection requires a 3D viewport. */
  if (t->region == nullptr || t->spacetype != SPACE_VIEW3D) {
    return;
  }

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  const Object *ob = BKE_view_layer_active_object_get(t->view_layer);
  const float (*ob_to_world)[4] = ob ? ob->object_to_world().ptr() : nullptr;

  /* Helpers: project an object-space point to 2D screen pixels. */
  auto project_to_screen = [&](const float3 &obj_co, float r_screen[2]) {
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
            i, j, screen_co, center_screen, world_co, point_radius, td2d, tdpc, td);
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
            i, 0, screen_co, center_screen, world_co, point_radius, td2d, tdpc, td);
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
            i, 2, screen_co, center_screen, world_co, point_radius, td2d, tdpc, td);
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

    /* Unproject the new 2D screen position back to world space using the original
     * point depth as the reference plane. */
    float new_world[3];
    if (t->mode == TFM_ROTATION && v3d && t->region) {
      /* `ElementRotation_ex` rotates around the transform orientation axis (world Z with Global
       * orientation), which is wrong for non-top views.  Instead, compute a pure 2D screen
       * rotation from the accumulated angle and the initial screen position, then unproject. */
      const TransData *td = &td_arr[i];
      const float angle = t->values_final[0];
      const float *center = transdata_check_local_center(t, t->around) ? td->center :
                                                                          tc->center_local;
      const float dx = td->iloc[0] - center[0];
      const float dy = td->iloc[1] - center[1];
      const float cos_a = cosf(angle);
      const float sin_a = sinf(angle);
      float screen_pos[2];
      screen_pos[0] = center[0] + dx * cos_a - dy * sin_a;
      screen_pos[1] = center[1] + dx * sin_a + dy * cos_a;
      ED_view3d_win_to_3d(v3d, t->region, tdpc->co_orig_world, screen_pos, new_world);
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
  }
}

/** \} */

TransConvertTypeInfo TransConvertType_PaintCurve = {
    /*flags*/ (T_POINTS | T_2D_EDIT),
    /*create_trans_data*/ createTransPaintCurveVerts,
    /*recalc_data*/ flushTransPaintCurve,
    /*special_aftertrans_update*/ nullptr,
};

}  // namespace blender::ed::transform
