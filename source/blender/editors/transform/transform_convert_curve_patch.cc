/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edtransform
 *
 * `transform.translate/rotate/resize` (G/R/S) support for a live Curve Patch's ACTIVE point.
 * Reuses #TransDataPaintCurve (`transform_convert.hh`) and the classic 3D-space math from
 * `transform_convert_paintcurve.cc`'s `flushTransPaintCurve()` -- Curve Patch's control curve is
 * always object-space (never the screen-bound 2D mode a `PaintCurve` can also be), so only that
 * one branch is needed here, copied rather than shared because the two flushes write back through
 * different accessors (a brush's `PaintCurve` geometry directly vs
 * #ED_curve_patch_session_active_point_handle_set) and #TFM_CURVE_SHRINKFATTEN is out of scope --
 * Alt+S's own radius drag (`paint_curve_patch_edit.cc`) already owns that.
 *
 * Always builds all three handles (left, pivot, right) of the active point, unconditionally --
 * unlike the classic path's per-handle `sel` bit-flag selection, G/R/S here always acts on the
 * whole active point, matching the "move/rotate/scale the active point" affordance the modal
 * editor's own point-drag already gives via direct mouse interaction.
 */

#include <cmath>

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"

#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_object_types.hh"

#include "ED_paint.hh"
#include "ED_view3d.hh"

#include "transform.hh"
#include "transform_convert.hh"
#include "transform_mode.hh"

namespace blender::ed::transform {

/* -------------------------------------------------------------------- */
/** \name Curve Patch Transform Creation
 * \{ */

static void createTransCurvePatchVerts(bContext * /*C*/, TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  tc->data_len = 0;

  if (t->spacetype != SPACE_VIEW3D || t->region == nullptr) {
    return;
  }

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  Object *ob = BKE_view_layer_active_object_get(t->view_layer);
  if (ob == nullptr) {
    return;
  }

  float3 obj_handles[3];
  for (int h = 0; h < 3; h++) {
    float co[3];
    if (!ED_curve_patch_session_active_point_handle_get(*ob, h, co)) {
      /* No running session, or no valid active point -- nothing to transform. */
      return;
    }
    obj_handles[h] = float3(co);
  }

  const float4x4 ob_to_world = ob->object_to_world();
  const float3 pivot_world = math::transform_point(ob_to_world, obj_handles[1]);
  float center_screen[2];
  ED_view3d_project_v2(t->region, pivot_world, center_screen);

  tc->data_len = 3;
  TransData2D *td2d = tc->data_2d = MEM_new_array_zeroed<TransData2D>(3, "TransData2D");
  TransData *td = tc->data = MEM_new_array_zeroed<TransData>(3, "TransData");
  TransDataPaintCurve *tdpc = static_cast<TransDataPaintCurve *>(
      tc->custom.type.data = MEM_new_array_zeroed<TransDataPaintCurve>(3, "TransDataPaintCurve"));
  tc->custom.type.use_free = true;

  for (int h = 0; h < 3; h++) {
    const float3 world_co = math::transform_point(ob_to_world, obj_handles[h]);
    float screen_co[2];
    ED_view3d_project_v2(t->region, world_co, screen_co);

    copy_v2_v2(td2d[h].loc, screen_co);
    td2d[h].loc[2] = 0.0f;
    /* Written back manually in the flush; skip generic 2D writeback. */
    td2d[h].loc2d = nullptr;

    td[h].flag = TD_SELECTED;
    td[h].loc = td2d[h].loc;
    td[h].center[0] = center_screen[0];
    td[h].center[1] = center_screen[1];
    td[h].center[2] = 0.0f;
    copy_v3_v3(td[h].iloc, td[h].loc);

    memset(td[h].axismtx, 0, sizeof(td[h].axismtx));
    td[h].axismtx[2][2] = 1.0f;

    unit_m3(td[h].mtx);
    unit_m3(td[h].smtx);

    td[h].dist = 0.0;
    /* No #TFM_CURVE_SHRINKFATTEN support here -- see the file header comment. */
    td[h].val = nullptr;

    /* Unused by the flush below, which always targets "the current active point" through
     * #ED_curve_patch_session_active_point_handle_set rather than an index into a local array --
     * kept zero for parity with #TransDataPaintCurve's other consumers. */
    tdpc[h].point_index = 0;
    tdpc[h].handle_index = h;
    tdpc[h].radius = 1.0f;
    copy_v3_v3(tdpc[h].co_orig_world, world_co);
    copy_v3_v3(tdpc[h].pivot_world, pivot_world);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve Patch Transform Flush
 * \{ */

static void flushTransCurvePatch(TransInfo *t)
{
  TransDataContainer *tc = TRANS_DATA_CONTAINER_FIRST_SINGLE(t);
  if (tc->data_len == 0) {
    return;
  }

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  Object *ob = BKE_view_layer_active_object_get(t->view_layer);
  if (ob == nullptr) {
    return;
  }

  View3D *v3d = (t->spacetype == SPACE_VIEW3D) ? static_cast<View3D *>(t->view) : nullptr;
  if (v3d == nullptr || t->region == nullptr) {
    return;
  }

  const float4x4 world_to_obj = ob->world_to_object();

  TransData2D *td2d = tc->data_2d;
  TransDataPaintCurve *tdpc = static_cast<TransDataPaintCurve *>(tc->custom.type.data);
  const TransData *td_arr = tc->data;

  /* Mirrors the 3D-space branch of `flushTransPaintCurve()` (`transform_convert_paintcurve.cc`)
   * verbatim -- see the file header comment for why this is a copy rather than a shared call. */
  for (int i = 0; i < tc->data_len; i++, td2d++, tdpc++) {
    float new_world[3];
    if (t->mode == TFM_ROTATION) {
      const bool has_axis_constraint = (t->con.mode & CON_APPLY) != 0;

      if (has_axis_constraint) {
        /* Axis-constrained rotation: apply a true 3D rotation in world space. */
        const float angle = t->values_final[0];

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

        float center_world[3];
        if (transdata_check_local_center(t, t->around)) {
          copy_v3_v3(center_world, tdpc->pivot_world);
        }
        else {
          zero_v3(center_world);
          const TransDataPaintCurve *tdpc_all = static_cast<const TransDataPaintCurve *>(
              tc->custom.type.data);
          for (int j = 0; j < tc->data_len; j++) {
            add_v3_v3(center_world, tdpc_all[j].co_orig_world);
          }
          mul_v3_fl(center_world, 1.0f / float(tc->data_len));
        }

        float rel[3];
        sub_v3_v3v3(rel, tdpc->co_orig_world, center_world);
        mul_v3_m3v3(new_world, rot_mat, rel);
        add_v3_v3(new_world, center_world);
      }
      else {
        /* No axis constraint: compute a pure 2D screen-plane rotation, then unproject. */
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
      float world_delta[3];
      mul_v3_m3v3(world_delta, t->spacemtx, t->values_final);

      if ((t->con.mode & CON_APPLY) && (t->con.mode & CON_AXIS2) &&
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
    else {
      /* TFM_RESIZE and anything else: `td2d->loc` already carries the generic constraint
       * pipeline's result (screen-space), unprojected here using the point's original depth. */
      ED_view3d_win_to_3d(v3d, t->region, tdpc->co_orig_world, td2d->loc, new_world);
    }

    const float3 new_obj = math::transform_point(world_to_obj, float3(new_world));
    const float co[3] = {new_obj.x, new_obj.y, new_obj.z};
    ED_curve_patch_session_active_point_handle_set(*ob, tdpc->handle_index, co);
  }

  if (t->context) {
    ED_curve_patch_session_restamp(*t->context, *ob);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Curve Patch Transform After
 * \{ */

static void specialAfterTransCurvePatch(bContext * /*C*/, TransInfo *t)
{
  if (t->state == TRANS_CANCEL) {
    /* The final `recalc_data` a cancel triggers already restored and re-stamped through the
     * flush above (`TransData::iloc`-driven), so there is nothing left to undo here. */
    return;
  }

  BKE_view_layer_synced_ensure(*t->bmain, t->scene, t->view_layer);
  Object *ob = BKE_view_layer_active_object_get(t->view_layer);
  if (ob == nullptr) {
    return;
  }
  /* One session-undo step for the whole drag, matching every other discrete Curve Patch edit
   * (point drag, radius drag, insert, delete...) -- never one per `recalc_data` tick, which would
   * otherwise flood the session's own Ctrl+Z stack with intermediate positions nobody wants to
   * step through. */
  ED_curve_patch_session_undo_push(*ob);
}

/** \} */

TransConvertTypeInfo TransConvertType_CurvePatch = {
    /*flags*/ (T_POINTS | T_2D_EDIT),
    /*create_trans_data*/ createTransCurvePatchVerts,
    /*recalc_data*/ flushTransCurvePatch,
    /*special_aftertrans_update*/ specialAfterTransCurvePatch,
};

}  // namespace blender::ed::transform
